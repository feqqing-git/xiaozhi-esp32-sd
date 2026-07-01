#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <cstdlib>
#include <vector>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <esp_sntp.h>
#include <sys/time.h>

#define TAG "Application"

static void InitializeNtpServers() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp1.aliyun.com");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "NTP initialized with ntp1.aliyun.com and ntp.aliyun.com");
}

static bool WaitForNtpSync(int timeout_seconds = 20) {
    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && retry < timeout_seconds) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "NTP sync completed");
        return true;
    }
    ESP_LOGW(TAG, "NTP sync failed after %d seconds", timeout_seconds);
    return false;
}

static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 3;
    int retry_count = 0;
    int retry_delay = 5; // 初始重试延迟为5秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Use Beijing time for all localtime conversions
    setenv("TZ", "CST-8", 1);
    tzset();

    /* Setup the display */
    auto display = board.GetDisplay();

    // Startup info is hidden to avoid showing board name/version on boot

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    /* Sync system time with NTP after network is ready */
    InitializeNtpServers();
    WaitForNtpSync(20);

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        
        // 初始化闹钟管理器
        auto& alarm_manager = AlarmManager::GetInstance();
        alarm_manager.Initialize();
        
        // 设置闹钟回调
        alarm_manager.SetAlarmTriggeredCallback([this](const AlarmItem& alarm) {
            Schedule([this, alarm]() { OnAlarmTriggered(alarm); });
        });
        alarm_manager.SetAlarmSnoozeCallback([this](const AlarmItem& alarm) {
            Schedule([this, alarm]() { OnAlarmSnoozed(alarm); });
        });
        alarm_manager.SetAlarmStopCallback([this](const AlarmItem& alarm) {
            Schedule([this, alarm]() { OnAlarmStopped(alarm); });
        });
        
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            display->OnClockTimer();
            
            // 检查闹钟（每秒检查一次）
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.CheckAlarms();
            
            // 更新音乐播放进度（每秒更新一次）
            if (is_music_playing_) {
                UpdateMusicProgress();
            }
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    display->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

// 新增：接收外部音频数据（如音乐播放）
void Application::AddAudioData(AudioStreamPacket&& packet) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (device_state_ == kDeviceStateIdle && codec->output_enabled()) {
        // packet.payload包含的是原始PCM数据（int16_t）
        if (packet.payload.size() >= 2) {
            size_t num_samples = packet.payload.size() / sizeof(int16_t);
            std::vector<int16_t> pcm_data(num_samples);
            memcpy(pcm_data.data(), packet.payload.data(), packet.payload.size());
            
            // 检查采样率是否匹配，如果不匹配则进行简单重采样
            if (packet.sample_rate != codec->output_sample_rate()) {
                // 验证采样率参数
                if (packet.sample_rate <= 0 || codec->output_sample_rate() <= 0) {
                    ESP_LOGE(TAG, "Invalid sample rates: %d -> %d", 
                            packet.sample_rate, codec->output_sample_rate());
                    return;
                }
                
                // 尝试动态切换采样率
                if (codec->SetOutputSampleRate(packet.sample_rate)) {
                    ESP_LOGI(TAG, "Successfully switched to music playback sampling rate: %d Hz", packet.sample_rate);
                } else {
                    ESP_LOGW(TAG, "Unable to switch sampling rate, continue using current sampling rate: %d Hz", codec->output_sample_rate());
                    // 如果无法切换采样率，继续使用当前的采样率进行处理
                    if (packet.sample_rate > codec->output_sample_rate()) {
                        // 下采样：简单丢弃部分样本
                        float downsample_ratio = static_cast<float>(packet.sample_rate) / codec->output_sample_rate();
                        size_t expected_size = static_cast<size_t>(pcm_data.size() / downsample_ratio + 0.5f);
                        std::vector<int16_t> resampled(expected_size);
                        size_t resampled_index = 0;
                        
                        for (size_t i = 0; i < pcm_data.size(); ++i) {
                            if (i % static_cast<size_t>(downsample_ratio) == 0) {
                                resampled[resampled_index++] = pcm_data[i];
                            }
                        }
                        
                        pcm_data = std::move(resampled);
                        ESP_LOGI(TAG, "Downsampled %d -> %d samples (ratio: %.2f)", 
                                pcm_data.size(), resampled.size(), downsample_ratio);
                    } else if (packet.sample_rate < codec->output_sample_rate()) {
                        // 上采样：线性插值
                        float upsample_ratio = codec->output_sample_rate() / static_cast<float>(packet.sample_rate);
                        size_t expected_size = static_cast<size_t>(pcm_data.size() * upsample_ratio + 0.5f);
                        std::vector<int16_t> resampled(expected_size);
                        
                        for (size_t i = 0; i < pcm_data.size(); ++i) {
                            // 添加原始样本
                            resampled[i * static_cast<size_t>(upsample_ratio)] = pcm_data[i];
                            
                            // 计算需要插值的样本数
                            int interpolation_count = static_cast<int>(upsample_ratio) - 1;
                            if (interpolation_count > 0 && i + 1 < pcm_data.size()) {
                                int16_t current = pcm_data[i];
                                int16_t next = pcm_data[i + 1];
                                for (int j = 1; j <= interpolation_count; ++j) {
                                    float t = static_cast<float>(j) / (interpolation_count + 1);
                                    int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                                    resampled[i * static_cast<size_t>(upsample_ratio) + j] = interpolated;
                                }
                            } else if (interpolation_count > 0) {
                                // 最后一个样本，直接重复
                                for (int j = 1; j <= interpolation_count; ++j) {
                                    resampled[i * static_cast<size_t>(upsample_ratio) + j] = pcm_data[i];
                                }
                            }
                        }
                        
                        pcm_data = std::move(resampled);
                        ESP_LOGI(TAG, "Upsampled %d -> %d samples (ratio: %.2f)", 
                                pcm_data.size() / static_cast<size_t>(upsample_ratio), pcm_data.size(), upsample_ratio);
                    }
                }
            }
            
            // 确保音频输出已启用
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
            
            // 发送PCM数据到音频编解码器
            codec->OutputData(pcm_data);
            
            audio_service_.UpdateOutputTimestamp();
        }
    }
}

// 随机闹钟音乐列表 - 流行、经典、适合早晨的歌曲
static const std::vector<std::string> DEFAULT_ALARM_SONGS = {
    "晴天", "七里香", "青花瓷", "稻香", "彩虹", "告白气球", "说好不哭", 
    "夜曲", "花海", "简单爱", "听妈妈的话", "东风破", "菊花台",
    "起风了", "红豆", "好久不见", "匆匆那年", "老男孩", "那些年",
    "小幸运", "成都", "南山南", "演员", "体面", "盗将行", "大鱼",
    "新不了情", "月亮代表我的心", "甜蜜蜜", "邓丽君", "我只在乎你",
    "友谊之光", "童年", "海阔天空", "光辉岁月", "真的爱你", "喜欢你",
    "突然好想你", "情非得已", "温柔", "倔强", "知足", "三个傻瓜",
    "恋爱循环", "千本樱", "打上花火", "lemon", "残酷天使的行动纲领",
    "鸟笼", "虹", "青鸟", "closer", "sugar", "shape of you", 
    "despacito", "perfect", "happier", "someone like you"
};

// 获取随机闹钟音乐
static std::string GetRandomAlarmMusic() {
    if (DEFAULT_ALARM_SONGS.empty()) {
        return "";
    }
    
    // 使用当前时间作为随机种子
    srand(esp_timer_get_time() / 1000000);
    size_t index = rand() % DEFAULT_ALARM_SONGS.size();
    return DEFAULT_ALARM_SONGS[index];
}

// 闹钟回调方法实现
void Application::OnAlarmTriggered(const AlarmItem& alarm) {
    ESP_LOGI("Application", "Alarm triggered: %s at %02d:%02d", 
             alarm.label.c_str(), alarm.hour, alarm.minute);
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto music = board.GetMusic();
    
    // 显示闹钟信息
    std::string alarm_message = "🎵 闹钟";
    if (!alarm.label.empty()) {
        alarm_message += "\n" + alarm.label;
    }
    alarm_message += "\n" + AlarmManager::FormatTime(alarm.hour, alarm.minute);
    
    // 优先在时钟界面显示（如果支持）
    display->ShowAlarmOnIdleScreen(alarm_message.c_str());
    
    // 同时设置聊天消息（作为备用）
    display->SetChatMessage("system", alarm_message.c_str());
    display->SetEmotion("music");
    
    // 确定要播放的音乐
    std::string music_to_play;
    if (!alarm.music_name.empty()) {
        // 使用用户指定的音乐
        music_to_play = alarm.music_name;
        ESP_LOGI("Application", "Playing user specified alarm music: %s", music_to_play.c_str());
    } else {
        // 随机选择一首默认闹钟音乐
        music_to_play = GetRandomAlarmMusic();
        ESP_LOGI("Application", "Playing random alarm music: %s", music_to_play.c_str());
    }
    
    // 播放音乐
    if (music && !music_to_play.empty()) {
        // 更新显示，显示正在播放的歌曲
        std::string playing_message = "🎵 正在播放: " + music_to_play;
        display->SetChatMessage("system", playing_message.c_str());
        
        // 开始下载并播放音乐
        if (music->Download(music_to_play)) {
            ESP_LOGI("Application", "Successfully started alarm music: %s", music_to_play.c_str());
            
            // 开始音乐进度跟踪
            current_music_name_ = music_to_play;
            music_start_time_ms_ = esp_timer_get_time() / 1000;  // 转换为毫秒
            is_music_playing_ = true;
            
            // 尝试获取真实的歌曲长度
            int real_duration = music->GetCurrentSongDurationSeconds();
            if (real_duration > 0) {
                music_duration_seconds_ = real_duration;
                ESP_LOGI("Application", "Got real song duration: %d seconds", real_duration);
            }
            
            // 启动进度显示
            display->SetMusicProgress(music_to_play.c_str(), 0, music_duration_seconds_, 0.0f);
        } else {
            ESP_LOGW("Application", "Failed to download alarm music: %s, using fallback", music_to_play.c_str());
            // 如果下载失败，播放默认铃声
            audio_service_.PlaySound(Lang::Sounds::OGG_VIBRATION);
        }
    } else {
        ESP_LOGW("Application", "Music service not available or no music selected, using default alarm sound");
        // 如果没有音乐功能或选择失败，播放默认铃声
        audio_service_.PlaySound(Lang::Sounds::OGG_VIBRATION);
    }
    
    // 显示闹钟控制提示
    std::string control_message = "🎵 说\"贪睡\"延后5分钟，说\"关闭闹钟\"停止音乐";
    display->ShowNotification(control_message.c_str());
}

void Application::OnAlarmSnoozed(const AlarmItem& alarm) {
    ESP_LOGI("Application", "Alarm snoozed: %s, count: %d/%d", 
             alarm.label.c_str(), alarm.snooze_count, alarm.max_snooze_count);
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // 隐藏空闲屏幕上的闹钟信息
    display->HideAlarmOnIdleScreen();
    
    // 停止当前播放的音乐
    auto music = board.GetMusic();
    if (music) {
        music->StopStreaming();
    }
    
    // 停止音乐进度跟踪并清除音乐界面
    is_music_playing_ = false;
    display->ClearMusicInfo();

    std::string snooze_message = "💤 闹钟已贪睡 " + std::to_string(alarm.snooze_minutes) + " 分钟";
    display->SetChatMessage("system", snooze_message.c_str());
    display->SetEmotion("neutral");

    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
}

void Application::OnAlarmStopped(const AlarmItem& alarm) {
    ESP_LOGI("Application", "Alarm stopped: %s", alarm.label.c_str());
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // 隐藏空闲屏幕上的闹钟信息
    display->HideAlarmOnIdleScreen();
    
    // 停止当前播放的音乐
    auto music = board.GetMusic();
    if (music) {
        music->StopStreaming();
    }
    
    // 停止音乐进度跟踪并清除音乐界面
    is_music_playing_ = false;
    display->ClearMusicInfo();

    display->SetChatMessage("system", "✅ 闹钟已关闭");
    display->SetEmotion("neutral");

    // 显示下一个闹钟信息
    auto& alarm_manager = AlarmManager::GetInstance();
    std::string next_alarm_info = alarm_manager.GetNextAlarmInfo();
    display->ShowNotification(next_alarm_info.c_str());

    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
}

// 获取默认闹钟音乐列表
std::vector<std::string> Application::GetDefaultAlarmMusicList() const {
    return DEFAULT_ALARM_SONGS;
}

// 更新音乐播放进度
void Application::UpdateMusicProgress() {
    if (!is_music_playing_) {
        return;
    }
    
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    auto display = board.GetDisplay();
    
    if (!music || !display) {
        return;
    }
    
    // 从音乐播放器获取真实的播放信息
    int real_current_seconds = music->GetCurrentPlayTimeSeconds();
    int real_duration_seconds = music->GetCurrentSongDurationSeconds();
    float real_progress_percent = music->GetPlayProgress();
    
    // 更新存储的歌曲长度（如果有变化）
    if (real_duration_seconds > 0 && real_duration_seconds != music_duration_seconds_) {
        music_duration_seconds_ = real_duration_seconds;
        ESP_LOGI("Application", "Updated song duration: %d seconds", music_duration_seconds_);
    }
    
    // 检查是否播放结束
    bool is_still_playing = music->IsDownloading() || (real_current_seconds < real_duration_seconds && real_current_seconds > 0);
    
    if (!is_still_playing && real_current_seconds >= real_duration_seconds && real_duration_seconds > 0) {
        is_music_playing_ = false;  // 停止跟踪
        ESP_LOGI("Application", "Music playback finished: %s (%d/%d seconds)", 
                 current_music_name_.c_str(), real_current_seconds, real_duration_seconds);
        
        // 🎵 音乐播放完毕，自动停止所有活跃的闹钟
        auto& alarm_manager = AlarmManager::GetInstance();
        auto active_alarms = alarm_manager.GetActiveAlarms();
        if (!active_alarms.empty()) {
            ESP_LOGI("Application", "Auto-stopping alarms after music finished");
            
            // 停止闹钟
            for (const auto& alarm : active_alarms) {
                alarm_manager.StopAlarm(alarm.id);
            }
            
            // 在界面上显示用户确认消息
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->SetChatMessage("user", "我听到你的闹钟啦 ✅");
                display->SetEmotion("happy");
            }
        }
    }
    
    // 更新显示（使用真实的播放时间）
    if (is_music_playing_) {
        display->SetMusicProgress(current_music_name_.c_str(), 
                                  real_current_seconds, 
                                  real_duration_seconds, 
                                  real_progress_percent);
        
        ESP_LOGD("Application", "Music progress: %s - %d/%d seconds (%.1f%%)", 
                 current_music_name_.c_str(), real_current_seconds, real_duration_seconds, real_progress_percent);
    } else {
        // 播放结束，清除界面
        display->ClearMusicInfo();
    }
}