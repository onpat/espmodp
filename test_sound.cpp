#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <xm.h>

int main() {
    std::ifstream file("main/roadblast.xm", std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (file.read(buffer.data(), size)) {
        xm_prescan_data_t prescan;
        xm_prescan_module(buffer.data(), size, &prescan);
        uint32_t ctx_size = xm_size_for_context(&prescan);
        char* pool = new char[ctx_size];
        xm_context_t* ctx = xm_create_context(pool, &prescan, buffer.data(), size);
        xm_set_sample_rate(ctx, 48000);
        float temp_buf[16];
        xm_generate_samples(ctx, temp_buf, 8);
        for(int i=0; i<16; i++) {
            std::cout << temp_buf[i] << " ";
        }
        std::cout << std::endl;
    }
    return 0;
}
