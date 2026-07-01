
#include "backgrounds.h"
#include <lvgl.h>

// 图像数据（需要你提供实际的图像数据）
const uint8_t background1_data[] = {};

const uint8_t background2_data[] = {};

const uint8_t background3_data[] = {};

// LVGL 9.x 图像描述符
const lv_img_dsc_t background_image_1 = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = 240,
        .h = 240,
        .stride = 480,
    },
    .data_size = 115200,  // 240 * 240 * 2
    .data = background1_data,
};

const lv_img_dsc_t background_image_2 = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = 240,
        .h = 240,
        .stride = 480,
    },
    .data_size = 115200,
    .data = background2_data,
};

const lv_img_dsc_t background_image_3 = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = 240,
        .h = 240,
        .stride = 480,
    },
    .data_size = 115200,
    .data = background3_data,
};

const lv_img_dsc_t* get_background_image(background_type_t type) {
    switch(type) {
        case BG_HOME: return &background_image_1;
        case BG_2: return &background_image_2;
        case BG_3: return &background_image_3;
        default: return &background_image_1;
    }
}

const char* get_background_name(background_type_t type) {
    switch(type) {
        case BG_HOME: return "主界面背景";
        case BG_2: return "背景1";
        case BG_3: return "背景2";
        default: return "默认背景";
    }
}
