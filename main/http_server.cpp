#include "http_server.hpp"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"

static const char* TAG = "HttpServer";

static const char* index_html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Control Panel</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: sans-serif; text-align: center; margin-top: 50px; }
        button { font-size: 20px; margin: 10px; padding: 10px 20px; }
        input { font-size: 20px; margin: 10px; padding: 10px; width: 80%; max-width: 300px; }
    </style>
    <script>
        function sendPost(action, data) {
            return fetch('/api/action', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ action: action, ...data })
            });
        }
        function startPlaying() { sendPost('start_playing', {}); }
        function stopPlaying() { sendPost('stop_playing', {}); }
        function displayString() {
            var msg = document.getElementById('msg').value;
            sendPost('display_string', { message: msg });
        }
        function loadFiles() {
            fetch('/api/files').then(r => r.json()).then(files => {
                const list = document.getElementById('fileList');
                list.innerHTML = '';
                files.forEach(f => {
                    const li = document.createElement('li');
                    li.innerHTML = '<a href="/api/download?filename=' + encodeURIComponent(f.name) + '">' + f.name + '</a> (' + f.size + ' bytes) <button onclick="loadXmFile(\'' + f.name + '\')">Load</button> <button onclick="deleteFile(\'' + f.name + '\')">Delete</button>';
                    list.appendChild(li);
                });
            });
        }
        function deleteFile(name) {
            sendPost('delete_file', { filename: name }).then(() => setTimeout(loadFiles, 500));
        }
        function loadXmFile(name) {
            sendPost('load_xm', { filename: name }).then(r => {
                if (!r.ok) {
                    r.json().then(data => alert('Error: ' + data.error)).catch(() => alert('Load failed'));
                } else {
                    alert('Loaded ' + name);
                }
            });
        }
        function uploadFile() {
            const fileInput = document.getElementById('fileInput');
            const file = fileInput.files[0];
            if (!file) return;
            if (file.size > 128 * 1024) {
                alert('File size exceeds 128KB limit!');
                return;
            }
            fetch('/api/upload?filename=' + encodeURIComponent(file.name), {
                method: 'POST',
                body: file
            }).then(r => {
                if (r.ok) {
                    fileInput.value = '';
                    loadFiles();
                } else alert('Upload failed');
            });
        }
        window.onload = loadFiles;
    </script>
</head>
<body>
    <h1>ESP32 Control Panel</h1>
    <button onclick="startPlaying()">Start Playing</button><br>
    <button onclick="stopPlaying()">Stop Playing</button><br>
    <hr>
    <input type="text" id="msg" placeholder="Message to display"><br>
    <button onclick="displayString()">Display String</button>
    <hr>
    <h2>Files</h2>
    <ul id="fileList" style="list-style-type: none; padding: 0;"></ul>
    <input type="file" id="fileInput"><br>
    <button onclick="uploadFile()">Upload File</button>
</body>
</html>
)rawliteral";

HttpServer::HttpServer() : server_handle_(nullptr) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::init_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    return ret == ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

bool HttpServer::init_wifi_ap(const std::string& ssid, const std::string& password) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid.c_str(), sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = ssid.length();
    strncpy((char*)wifi_config.ap.password, password.c_str(), sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_ap finished. SSID:%s password:%s", ssid.c_str(), password.c_str());
    return true;
}

bool HttpServer::init_wifi_sta(const std::string& ssid, const std::string& password) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished. SSID:%s password:%s", ssid.c_str(), password.c_str());
    return true;
}

int HttpServer::index_html_get_handler(void* req_v) {
    httpd_req_t *req = (httpd_req_t *)req_v;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

int HttpServer::api_post_handler(void* req_v) {
    httpd_req_t *req = (httpd_req_t *)req_v;
    HttpServer* server = (HttpServer*)req->user_ctx;
    
    char buf[256];
    int ret, remaining = req->content_len;
    std::string body;

    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, std::min((int)sizeof(buf), remaining))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        body.append(buf, ret);
        remaining -= ret;
    }

    cJSON *json = cJSON_Parse(body.c_str());
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (cJSON_IsString(action) && (action->valuestring != NULL)) {
        if (strcmp(action->valuestring, "start_playing") == 0) {
            if (server->callbacks_.on_start_playing) {
                server->callbacks_.on_start_playing();
            }
        } else if (strcmp(action->valuestring, "stop_playing") == 0) {
            if (server->callbacks_.on_stop_playing) {
                server->callbacks_.on_stop_playing();
            }
        } else if (strcmp(action->valuestring, "display_string") == 0) {
            cJSON *msg = cJSON_GetObjectItem(json, "message");
            if (cJSON_IsString(msg) && (msg->valuestring != NULL)) {
                if (server->callbacks_.on_display_string) {
                    server->callbacks_.on_display_string(msg->valuestring);
                }
            }
        } else if (strcmp(action->valuestring, "delete_file") == 0) {
            cJSON *filename = cJSON_GetObjectItem(json, "filename");
            if (cJSON_IsString(filename) && (filename->valuestring != NULL)) {
                std::string path = std::string("/lfs/") + filename->valuestring;
                unlink(path.c_str());
            }
        } else if (strcmp(action->valuestring, "load_xm") == 0) {
            cJSON *filename = cJSON_GetObjectItem(json, "filename");
            if (cJSON_IsString(filename) && (filename->valuestring != NULL)) {
                std::string path = std::string("/lfs/") + filename->valuestring;
                if (server->callbacks_.on_load_xm) {
                    if (!server->callbacks_.on_load_xm(path)) {
                        cJSON_Delete(json);
                        httpd_resp_set_status(req, HTTPD_500);
                        httpd_resp_set_type(req, "application/json");
                        httpd_resp_send(req, "{\"error\":\"Load failed or out of memory\"}", HTTPD_RESP_USE_STRLEN);
                        return ESP_OK;
                    }
                }
            }
        }
    }

    cJSON_Delete(json);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

bool HttpServer::start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = (esp_err_t (*)(httpd_req_t *))index_html_get_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_post = {
            .uri       = "/api/action",
            .method    = HTTP_POST,
            .handler   = (esp_err_t (*)(httpd_req_t *))api_post_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_post);

        httpd_uri_t uri_files = {
            .uri       = "/api/files",
            .method    = HTTP_GET,
            .handler   = (esp_err_t (*)(httpd_req_t *))api_files_get_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_files);

        httpd_uri_t uri_upload = {
            .uri       = "/api/upload",
            .method    = HTTP_POST,
            .handler   = (esp_err_t (*)(httpd_req_t *))api_upload_post_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_upload);

        httpd_uri_t uri_download = {
            .uri       = "/api/download",
            .method    = HTTP_GET,
            .handler   = (esp_err_t (*)(httpd_req_t *))api_download_get_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_download);

        server_handle_ = server;
        return true;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return false;
}

bool HttpServer::start(Mode mode, const Callbacks& callbacks, const std::string& ssid, const std::string& password) {
    callbacks_ = callbacks;

    init_nvs();

    if (mode == Mode::AP) {
        init_wifi_ap(ssid, password);
    } else {
        init_wifi_sta(ssid, password);
    }

    return start_webserver();
}

void HttpServer::stop() {
    if (server_handle_) {
        httpd_stop((httpd_handle_t)server_handle_);
        server_handle_ = nullptr;
    }
}

int HttpServer::api_files_get_handler(void* req_v) {
    httpd_req_t *req = (httpd_req_t *)req_v;
    DIR* dir = opendir("/lfs");
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }
    
    cJSON* array = cJSON_CreateArray();
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        std::string path = std::string("/lfs/") + entry->d_name;
        if (stat(path.c_str(), &st) == 0) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", entry->d_name);
            cJSON_AddNumberToObject(item, "size", st.st_size);
            cJSON_AddItemToArray(array, item);
        }
    }
    closedir(dir);
    
    char* json_str = cJSON_PrintUnformatted(array);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    
    free(json_str);
    cJSON_Delete(array);
    return ESP_OK;
}

int HttpServer::api_upload_post_handler(void* req_v) {
    httpd_req_t *req = (httpd_req_t *)req_v;
    
    char filename[128] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char* query = (char*)malloc(query_len + 1);
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            httpd_query_key_value(query, "filename", filename, sizeof(filename));
        }
        free(query);
    }
    
    if (strlen(filename) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }
    
    std::string path = std::string("/lfs/") + filename;
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
        return ESP_FAIL;
    }
    
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, std::min((int)sizeof(buf), remaining));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            return ESP_FAIL;
        }
        fwrite(buf, 1, ret, f);
        remaining -= ret;
    }
    fclose(f);
    
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

int HttpServer::api_download_get_handler(void* req_v) {
    httpd_req_t *req = (httpd_req_t *)req_v;

    char filename[128] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char* query = (char*)malloc(query_len + 1);
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            httpd_query_key_value(query, "filename", filename, sizeof(filename));
        }
        free(query);
    }

    if (strlen(filename) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }

    std::string path = std::string("/lfs/") + filename;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    std::string disposition = "attachment; filename=\"" + std::string(filename) + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());

    char chunk[1024];
    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, sizeof(chunk), f);
        if (chunksize > 0) {
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                fclose(f);
                return ESP_FAIL;
            }
        }
    } while (chunksize != 0);

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
