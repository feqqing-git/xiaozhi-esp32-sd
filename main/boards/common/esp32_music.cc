#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "server_config.h"
#include "device_manager.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <thread>   // 为线程ID比较
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dirent.h>
#include <sys/stat.h>
#include <functional>
#include <cstdio>
#include <memory>
#include <vector>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdmmc_defs.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>

#define TAG "Esp32Music"

// ========== 简单的ESP32认证函数 ==========

/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id() {
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥
 * @param timestamp 时间戳
 * @return 动态密钥字符串
 */
static std::string generate_dynamic_key(int64_t timestamp) {
    // 密钥（请修改为与服务端一致）
    const std::string secret_key = "your-esp32-secret-key-2024";
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 组合数据：MAC:芯片ID:时间戳:密钥
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    
    // SHA256哈希
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    
    // 转换为十六进制字符串（前16字节）
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    
    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 * @param http HTTP客户端指针
 */
static void add_auth_headers(Http* http) {
    // 获取当前时间戳
    int64_t timestamp = esp_timer_get_time() / 1000000;  // 转换为秒
    
    // 生成动态密钥
    std::string dynamic_key = generate_dynamic_key(timestamp);
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 添加认证头
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        
        // 获取并添加设备Token
        auto& device_manager = DeviceManager::GetInstance();
        std::string token = device_manager.GetDeviceToken();
        if (!token.empty()) {
            http->SetHeader("X-Device-Token", token);
            ESP_LOGI(TAG, "Added X-Device-Token: %s...", token.substr(0, 8).c_str());
        }
        
        ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld", 
                 mac.c_str(), chip_id.c_str(), timestamp);
    }
}

static std::string ToLowerCopy(const std::string& input) {
    std::string output = input;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return output;
}

static bool IsSupportedAudioFile(const std::string& file_name) {
    std::string lower_name = ToLowerCopy(file_name);
    return lower_name.find(".mp3") != std::string::npos ||
           lower_name.find(".wav") != std::string::npos ||
           lower_name.find(".ogg") != std::string::npos;
}

static std::string NormalizeSearchText(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (unsigned char c : input) {
        if (std::isalnum(c)) {
            output.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == 0 || c >= 0x80) {
            // Preserve UTF-8 multibyte characters such as Chinese.
            output.push_back(static_cast<char>(c));
        } else if (std::isspace(c) || c == '-' || c == '_' || c == '/' || c == '.' || c == '(' || c == ')' ||
                   c == '[' || c == ']' || c == '{' || c == '}' || c == '&' || c == '+' || c == '@' || c == '#') {
            if (!output.empty() && output.back() != ' ') {
                output.push_back(' ');
            }
        }
    }
    while (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }
    return output;
}

static std::vector<std::string> TokenizeNormalized(const std::string& normalized_text) {
    std::vector<std::string> tokens;
    std::stringstream stream(normalized_text);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

static bool ContainsAllTokens(const std::string& text, const std::vector<std::string>& tokens) {
    for (const auto& token : tokens) {
        if (token.empty()) {
            continue;
        }
        if (text.find(token) == std::string::npos) {
            return false;
        }
    }
    return true;
}

static void SplitSongTitleArtist(const std::string& base_name, std::string& out_title, std::string& out_artist) {
    static const std::vector<std::string> separators = {" - ", "－", "–", "—", "~", "|", "_"};
    for (const auto& sep : separators) {
        size_t pos = base_name.find(sep);
        if (pos != std::string::npos) {
            out_artist = base_name.substr(0, pos);
            out_title = base_name.substr(pos + sep.size());
            return;
        }
    }
    out_title = base_name;
    out_artist.clear();
}

static int ComputeSearchScore(const std::string& normalized_title,
                              const std::string& normalized_artist,
                              const std::string& normalized_query_title,
                              const std::string& normalized_query_artist) {
    if (normalized_title.empty() && normalized_artist.empty()) {
        return 0;
    }

    std::vector<std::string> title_tokens = TokenizeNormalized(normalized_query_title);
    std::vector<std::string> artist_tokens = TokenizeNormalized(normalized_query_artist);

    int score = 0;
    bool title_exact = !normalized_query_title.empty() && normalized_title == normalized_query_title;
    bool artist_exact = !normalized_query_artist.empty() && normalized_artist == normalized_query_artist;
    bool title_contains = !normalized_query_title.empty() && normalized_title.find(normalized_query_title) != std::string::npos;
    bool artist_contains = !normalized_query_artist.empty() && normalized_artist.find(normalized_query_artist) != std::string::npos;
    bool title_tokens_match = !title_tokens.empty() && ContainsAllTokens(normalized_title, title_tokens);
    bool artist_tokens_match = !artist_tokens.empty() && ContainsAllTokens(normalized_artist, artist_tokens);
    bool title_artist_contains = !normalized_query_title.empty() &&
                                 (normalized_title + " " + normalized_artist).find(normalized_query_title) != std::string::npos;

    if (title_exact) {
        score += 1000;
    } else if (title_contains) {
        score += 600;
    } else if (title_tokens_match) {
        score += 400;
    }

    if (artist_exact) {
        score += 800;
    } else if (artist_contains) {
        score += 400;
    } else if (artist_tokens_match) {
        score += 250;
    }

    if (!normalized_query_title.empty() && title_artist_contains) {
        score += 100;
    }

    if (!title_tokens.empty() && title_tokens_match) {
        score += 100;
    }
    if (!artist_tokens.empty() && artist_tokens_match) {
        score += 100;
    }

    return score;
}

static bool ScanSdCardForMusic(const std::string& root_dir, const std::string& song_name,
                               const std::string& artist_name, std::string& out_path) {
    int best_score = 0;
    std::string best_match;

    std::string normalized_query_title = NormalizeSearchText(song_name);
    std::string normalized_query_artist = NormalizeSearchText(artist_name);

    std::function<void(const std::string&)> scan_dir = [&](const std::string& dir) {
        DIR* dir_handle = opendir(dir.c_str());
        if (!dir_handle) {
            return;
        }

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir_handle)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string full_path = dir + "/" + entry->d_name;
            struct stat st;
            if (stat(full_path.c_str(), &st) != 0) {
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                scan_dir(full_path);
                continue;
            }

            if (!S_ISREG(st.st_mode)) {
                continue;
            }

            std::string lower_name = ToLowerCopy(entry->d_name);
            if (!IsSupportedAudioFile(lower_name)) {
                continue;
            }

            std::string file_name_without_ext = entry->d_name;
            size_t dot_pos = file_name_without_ext.find_last_of('.');
            if (dot_pos != std::string::npos) {
                file_name_without_ext.erase(dot_pos);
            }

            std::string normalized_file_name = NormalizeSearchText(file_name_without_ext);
            std::string title;
            std::string artist;
            SplitSongTitleArtist(normalized_file_name, title, artist);

            int score = ComputeSearchScore(title, artist, normalized_query_title, normalized_query_artist);
            if (score > best_score || (score == best_score && !best_match.empty() && file_name_without_ext.size() < best_match.size())) {
                best_score = score;
                best_match = full_path;
            }
        }

        closedir(dir_handle);
    };

    std::vector<std::string> candidate_roots = {"/sdcard", "/sdcard/music", "/sdcard/audio", "/sdcard/songs"};
    for (const auto& candidate : candidate_roots) {
        scan_dir(candidate);
    }

    if (best_score > 0) {
        out_path = best_match;
        return true;
    }

    return false;
}

static bool MountSdCardIfNeeded() {
    struct stat st;
    if (stat("/sdcard", &st) == 0) {
        return true;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = GPIO_NUM_39;
    slot_config.cmd = GPIO_NUM_38;
    slot_config.d0 = GPIO_NUM_40;
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    memset(&mount_config, 0, sizeof(mount_config));
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = false;
   

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted successfully on CMD=%d CLK=%d DATA=%d", 38, 39, 40);
    return true;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         display_mode_(DISPLAY_MODE_SPECTRUM), is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), current_play_time_ms_(0), 
                         last_frame_time_ms_(0), total_frames_decoded_(0), current_song_duration_seconds_(0),
                         audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false), playlist_(), playlist_mutex_(),
                         current_playlist_index_(-1), playlist_mode_(false), playlist_thread_() {
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    playlist_mode_ = false;
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();
        
        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }
            
            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!download_thread_.joinable()) {
                thread_finished = true;
            }
            
            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0) {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }
        
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }
    
    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();
        
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }
            
            // 再次设置停止标志
            is_playing_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!play_thread_.joinable()) {
                thread_finished = true;
            }
        }
        
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }
    
    // 等待歌词线程结束
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }
    
    // 等待播放队列线程结束
    if (playlist_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playlist thread to finish");
        playlist_thread_.join();
        ESP_LOGI(TAG, "Playlist thread finished");
    }
    
    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "Starting SD card music playback for: %s", song_name.c_str());

    last_downloaded_data_.clear();
    current_song_name_ = song_name;
    current_music_url_.clear();
    current_lyric_url_.clear();
    current_song_duration_seconds_ = 0;

    if (!MountSdCardIfNeeded()) {
        ESP_LOGE(TAG, "SD card is not available, cannot play local music");
        return false;
    }

    std::string music_path;
    if (!ScanSdCardForMusic("/sdcard", song_name, artist_name, music_path)) {
        ESP_LOGE(TAG, "Failed to find music file for '%s' on SD card", song_name.c_str());
        return false;
    }

    current_music_url_ = music_path;
    song_name_displayed_ = false;
    ESP_LOGI(TAG, "Using local SD card audio file: %s", music_path.c_str());

    return StartStreaming(current_music_url_);
}



std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    // 停止之前的播放和下载
    is_downloading_ = false;
    is_playing_ = false;
    
    // 等待之前的线程完全结束
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        play_thread_.join();
    }
    
    // 清空缓冲区
    ClearAudioBuffer();
    
    // 配置线程栈大小以避免栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;  // 8KB栈大小
    cfg.prio = 5;           // 中等优先级
    cfg.thread_name = "audio_stream";
    esp_pthread_set_cfg(&cfg);
    
    // 开始下载线程
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    
    // 开始播放线程（会等待缓冲区有足够数据）
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    // 重置采样率到原始值
    ResetSampleRate();
    
    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }
    
    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;
    
    // 清空歌名显示
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");  // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display");
    }
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待线程结束（避免重复代码，让StopStreaming也能等待线程完全停止）
    if (download_thread_.joinable()) {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread joined in StopStreaming");
    }
    
    // 等待播放线程结束，使用更安全的方式
    if (play_thread_.joinable()) {
        // 先设置停止标志
        is_playing_ = false;
        
        // 通知条件变量，确保线程能够退出
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        // 使用超时机制等待线程结束，避免死锁
        bool thread_finished = false;
        int wait_count = 0;
        const int max_wait = 100; // 最多等待1秒
        
        while (!thread_finished && wait_count < max_wait) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
            
            // 检查线程是否仍然可join
            if (!play_thread_.joinable()) {
                thread_finished = true;
                break;
            }
        }
        
        if (play_thread_.joinable()) {
            if (wait_count >= max_wait) {
                ESP_LOGW(TAG, "Play thread join timeout, detaching thread");
                play_thread_.detach();
            } else {
                play_thread_.join();
                ESP_LOGI(TAG, "Play thread joined in StopStreaming");
            }
        }
    }
    
    // 在线程完全结束后，只在频谱模式下停止FFT显示
    if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
        display->stopFft();
        ESP_LOGI(TAG, "Stopped FFT display in StopStreaming (spectrum mode)");
    } else if (display) {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop in StopStreaming");
    }
    
    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting local SD-card audio stream from: %s", music_url.c_str());

    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music source path is empty");
        is_downloading_ = false;
        return;
    }

    const size_t chunk_size = 4096;
    char buffer[chunk_size];
    size_t total_downloaded = 0;

    ESP_LOGI(TAG, "Reading audio from local SD card file: %s", music_url.c_str());
    FILE* file = fopen(music_url.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open local audio file: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }

    while (is_downloading_ && is_playing_) {
        size_t bytes_read = fread(buffer, 1, chunk_size, file);
        if (bytes_read == 0) {
            if (ferror(file)) {
                ESP_LOGE(TAG, "Failed to read local audio file: %s", music_url.c_str());
            }
            break;
        }

        if (total_downloaded == 0 && bytes_read >= 4) {
            if (memcmp(buffer, "ID3", 3) == 0) {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            } else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0) {
                ESP_LOGI(TAG, "Detected MP3 file header");
            }
        }

        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);

        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });

            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                buffer_cv_.notify_one();
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }

    fclose(file);

    is_downloading_ = false;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        ESP_LOGE(TAG, "Audio codec not available or not enabled");
        is_playing_ = false;
        return;
    }
    
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        return;
    }
    
    
    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); 
        });
    }
    
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // 分配MP3输入缓冲区
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;
    
    while (is_playing_) {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            if (current_state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "Device is in speaking state, switching to listening state for music playback");
            }
            if (current_state == kDeviceStateListening) {
                ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            }
            // 切换状态
            app.ToggleChatState(); // 变成待机状态
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                // 格式化歌名显示为《歌名》播放中...
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display) {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
                    display->start();
                    ESP_LOGI(TAG, "Display start() called for spectrum visualization");
                } else {
                    ESP_LOGI(TAG, "Lyrics display mode active, FFT visualization disabled");
                }
            }
        }
        
        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码
            AudioChunk chunk;
            
            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                
                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }
            
            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0) {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }
        
        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }
        
        // 跳过到同步位置
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // 解码MP3帧
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
        if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;
            
            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
                    
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                            mp3_frame_info_.nChans);
                }
                
                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60;  // 使用Application默认的帧时长
                packet.timestamp = 0;
                
                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                if (final_pcm_data_fft == nullptr) {
                    final_pcm_data_fft = (int16_t*)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM
                    );
                }
                
                memcpy(
                    final_pcm_data_fft,
                    final_pcm_data,
                    final_sample_count * sizeof(int16_t)
                );
                
                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // 发送到Application的音频解码队列
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
                
                // 打印播放进度
                if (total_played % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
            
        } else {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);
            
            // 跳过一些字节继续尝试
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
            } else {
                bytes_left = 0;
            }
        }
    }
    
    // 清理
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
    }
    
    // 播放结束时进行基本清理，但不调用StopStreaming避免线程自我等待
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    ESP_LOGI(TAG, "Performing basic cleanup from play thread");
    
    // 停止播放标志
    is_playing_ = false;
    
    // 只在频谱显示模式下才停止FFT显示
    if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->stopFft();
            ESP_LOGI(TAG, "Stopped FFT display from play thread (spectrum mode)");
        }
    } else {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop");
    }

    // 播放结束后确保恢复原始采样率
    ResetSampleRate();
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 && 
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
                codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {  // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    // 检查URL是否为空
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }
    
    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // 最多允许5次重定向
    
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }
        
        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        
        // 添加ESP32认证头
        add_auth_headers(http.get());
        
        // 打开GET连接
        
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }
        
        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);
        
        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        
        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");
        
        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
                
                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0) {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            } else if (bytes_read == 0) {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }
        
        http->Close();
        
        if (read_error) {
            retry_count++;
            continue;
        }
        
        // 如果成功读取数据，跳出重试循环
        if (success) {
            break;
        }
    }
    
    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }
    
    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty()) {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content");
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    
    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format) {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        } else {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    
    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text;
            
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            
            // 显示歌词
            display->SetChatMessage("lyric", lyric_text.c_str());
            
            ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
                    current_time_ms, 
                    lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

// 删除复杂的认证初始化方法，使用简单的静态函数

// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 * 
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode) {
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;
    
    ESP_LOGI(TAG, "Display mode changed from %s to %s", 
            (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
            (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");
}

// ========== 播放队列功能实现 ==========

bool Esp32Music::PlayPlaylist(const std::vector<SongInfo>& songs) {
    if (songs.empty()) {
        ESP_LOGW(TAG, "Playlist is empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting playlist with %d songs", (int)songs.size());
    
    // 停止当前播放
    StopPlaylist();
    
    // 设置播放队列
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        playlist_ = songs;
        current_playlist_index_ = 0;
        playlist_mode_ = true;
    }
    
    // 启动播放队列管理线程
    playlist_thread_ = std::thread(&Esp32Music::PlaylistManagerThread, this);
    
    return true;
}

bool Esp32Music::NextSong() {
    if (!playlist_mode_.load()) {
        ESP_LOGW(TAG, "Not in playlist mode");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (current_playlist_index_ + 1 < (int)playlist_.size()) {
        current_playlist_index_++;
        ESP_LOGI(TAG, "Moving to next song: %d/%d", current_playlist_index_ + 1, (int)playlist_.size());
        return true;
    } else {
        ESP_LOGI(TAG, "Reached end of playlist");
        return false;
    }
}

bool Esp32Music::PreviousSong() {
    if (!playlist_mode_.load()) {
        ESP_LOGW(TAG, "Not in playlist mode");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (current_playlist_index_ > 0) {
        current_playlist_index_--;
        ESP_LOGI(TAG, "Moving to previous song: %d/%d", current_playlist_index_ + 1, (int)playlist_.size());
        return true;
    } else {
        ESP_LOGI(TAG, "Already at first song");
        return false;
    }
}

void Esp32Music::StopPlaylist() {
    ESP_LOGI(TAG, "Stopping playlist");
    
    playlist_mode_ = false;
    
    // 停止当前播放
    StopStreaming();
    
    // 等待播放队列线程结束
    if (playlist_thread_.joinable()) {
        playlist_thread_.join();
    }
    
    // 清空播放队列
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        playlist_.clear();
        current_playlist_index_ = -1;
    }
}

size_t Esp32Music::GetPlaylistSize() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    return playlist_.size();
}

SongInfo Esp32Music::GetCurrentSong() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (current_playlist_index_ >= 0 && current_playlist_index_ < (int)playlist_.size()) {
        return playlist_[current_playlist_index_];
    }
    return SongInfo();
}

void Esp32Music::PlaylistManagerThread() {
    ESP_LOGI(TAG, "Playlist manager thread started");
    
    while (playlist_mode_.load()) {
        SongInfo current_song;
        
        // 获取当前要播放的歌曲
        {
            std::lock_guard<std::mutex> lock(playlist_mutex_);
            if (current_playlist_index_ >= 0 && current_playlist_index_ < (int)playlist_.size()) {
                current_song = playlist_[current_playlist_index_];
            } else {
                ESP_LOGI(TAG, "Playlist finished");
                break;
            }
        }
        
        // 播放当前歌曲
        ESP_LOGI(TAG, "Playing song %d/%d: %s - %s", 
                current_playlist_index_ + 1, (int)GetPlaylistSize(),
                current_song.title.c_str(), current_song.artist.c_str());
        
        PlayCurrentSong();
        
        // 等待当前歌曲播放完成
        while (playlist_mode_.load() && (is_playing_.load() || is_downloading_.load())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        if (!playlist_mode_.load()) {
            break;
        }
        
        // 自动播放下一首
        if (!NextSong()) {
            ESP_LOGI(TAG, "Playlist completed");
            break;
        }
    }
    
    ESP_LOGI(TAG, "Playlist manager thread finished");
    playlist_mode_ = false;
}

void Esp32Music::PlayCurrentSong() {
    SongInfo song = GetCurrentSong();
    if (song.title.empty()) {
        ESP_LOGE(TAG, "No current song to play");
        return;
    }
    
    // 调用现有的Download方法播放歌曲
    if (!Download(song.title, song.artist)) {
        ESP_LOGE(TAG, "Failed to play song: %s - %s", song.title.c_str(), song.artist.c_str());
    }
}

bool Esp32Music::PlaySongByPath(const std::string& file_path, const std::string& song_name) {
    if (file_path.empty()) {
        ESP_LOGE(TAG, "PlaySongByPath called with empty file path");
        return false;
    }

    if (!MountSdCardIfNeeded()) {
        ESP_LOGE(TAG, "SD card not available for PlaySongByPath");
        return false;
    }

    last_downloaded_data_.clear();
    current_music_url_ = file_path;
    current_song_name_ = song_name;
    current_lyric_url_.clear();
    current_song_duration_seconds_ = 0;
    song_name_displayed_ = false;

    ESP_LOGI(TAG, "Playing local song by path: %s", file_path.c_str());
    return StartStreaming(file_path);
}

std::vector<SongInfo> Esp32Music::GetAvailableSongs() const {
    std::vector<SongInfo> songs;
    if (!MountSdCardIfNeeded()) {
        return songs;
    }

    std::function<void(const std::string&)> scan_dir = [&](const std::string& dir) {
        DIR* dir_handle = opendir(dir.c_str());
        if (!dir_handle) {
            return;
        }

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir_handle)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string full_path = dir + "/" + entry->d_name;
            struct stat st;
            if (stat(full_path.c_str(), &st) != 0) {
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                scan_dir(full_path);
                continue;
            }

            if (!S_ISREG(st.st_mode)) {
                continue;
            }

            std::string lower_name = ToLowerCopy(entry->d_name);
            if (!IsSupportedAudioFile(lower_name)) {
                continue;
            }

            std::string title = entry->d_name;
            std::string artist;
            size_t dot_pos = title.find_last_of('.');
            if (dot_pos != std::string::npos) {
                title.erase(dot_pos);
            }

            size_t separator_pos = title.find(" - ");
            if (separator_pos == std::string::npos) {
                separator_pos = title.find("－");
            }
            if (separator_pos != std::string::npos) {
                artist = title.substr(0, separator_pos);
                title = title.substr(separator_pos + 3);
            }

            songs.emplace_back(title, artist, full_path);
        }

        closedir(dir_handle);
    };

    std::vector<std::string> candidate_roots = {"/sdcard", "/sdcard/music", "/sdcard/audio", "/sdcard/songs"};
    for (const auto& candidate : candidate_roots) {
        scan_dir(candidate);
    }

    return songs;
}