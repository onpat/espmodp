#pragma once

#include <functional>
#include <string>

class HttpServer {
public:
    struct Callbacks {
        std::function<void()> on_start_playing;
        std::function<void()> on_stop_playing;
        std::function<void(const std::string&)> on_display_string;
        std::function<bool(const std::string&)> on_load_xm;
    };

    enum class Mode {
        AP,
        Client
    };

    HttpServer();
    ~HttpServer();

    // Disable copy/move
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Start WiFi and HTTP server
    bool start(Mode mode, const Callbacks& callbacks, const std::string& ssid = "ESP32-AP", const std::string& password = "");
    void stop();

private:
    Callbacks callbacks_;
    void* server_handle_; // httpd_handle_t
    
    bool init_nvs();
    bool init_wifi_ap(const std::string& ssid, const std::string& password);
    bool init_wifi_sta(const std::string& ssid, const std::string& password);
    bool start_webserver();
    
    // Internal handlers
    static int index_html_get_handler(void* req);
    static int api_post_handler(void* req);
    static int api_files_get_handler(void* req);
    static int api_upload_post_handler(void* req);
    static int api_download_get_handler(void* req);
};
