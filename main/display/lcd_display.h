#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <array>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    
    // 音乐播放界面UI元素
    lv_obj_t* music_panel_ = nullptr;           // 音乐播放面板
    lv_obj_t* music_title_label_ = nullptr;     // 歌曲名称标签
    lv_obj_t* music_time_label_ = nullptr;      // 时间显示标签
    lv_obj_t* music_progress_bar_ = nullptr;    // 进度条
    lv_obj_t* music_progress_bg_ = nullptr;     // 进度条背景
    lv_obj_t* music_vinyl_record_ = nullptr;    // 旋转唱片
    lv_obj_t* music_vinyl_center_ = nullptr;    // 唱片中心圆点
    lv_obj_t* music_vinyl_arm_ = nullptr;       // 唱片臂（可选）
    std::array<lv_obj_t*, 16> music_spectrum_bars_{}; // 底部频谱柱
    std::array<int, 16> music_spectrum_levels_{}; // 最近一帧频谱高度
    lv_anim_t* vinyl_rotation_anim_ = nullptr;  // 旋转动画
    lv_timer_t* music_spectrum_timer_ = nullptr; // 频谱动画定时器
    int music_spectrum_phase_ = 0;              // 频谱动画相位
    bool music_panel_visible_ = false;          // 音乐面板是否可见

    esp_timer_handle_t background_carousel_timer_ = nullptr;
    int background_index_ = 0;
    std::unique_ptr<LvglImage> background_image_cached_ = nullptr;

    void InitializeLcdThemes();
    void SetupUI();
    void SetupMusicPanel();  // 初始化音乐播放面板
    void SetupMusicSpectrumBars();
    void UpdateMusicSpectrumBars();
    static void MusicSpectrumTimerCb(lv_timer_t* timer);
    void UpdateBackgroundCarousel();
    void SetBackgroundCarouselImage(int index);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Add music display functions
    virtual void SetMusicInfo(const char* song_name) override;
    virtual void SetMusicProgress(const char* song_name, int current_seconds, int total_seconds, float progress_percent) override;
    virtual void ClearMusicInfo() override;
    virtual void start() override;
    virtual void clearScreen() override;
    virtual void stopFft() override;
};

// SPI LCD显示器
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD显示器
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD显示器
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
