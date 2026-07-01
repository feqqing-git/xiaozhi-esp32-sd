#pragma once

#include <lvgl.h>

#define BACKGROUND_COUNT 3

typedef enum {
    BG_HOME = 0,
    BG_2 = 1,
    BG_3 = 2,
} background_type_t;

extern const uint8_t background1_data[];
extern const uint8_t background2_data[];
extern const uint8_t background3_data[];

extern const lv_img_dsc_t background_image_1;
extern const lv_img_dsc_t background_image_2;
extern const lv_img_dsc_t background_image_3;

const lv_img_dsc_t* get_background_image(background_type_t type);
const char* get_background_name(background_type_t type);
