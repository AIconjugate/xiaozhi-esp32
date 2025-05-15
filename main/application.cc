/**************************************************************************************************************

这份文件是 `Application` 类（在 `application.h` 中声明）的具体实现，构成了小智 ESP32 项目的应用层核心逻辑。

**整体概览**

`application.cc` 主要负责：

1.  **初始化**: 设置系统组件、硬件、网络、音频处理、通信协议等。
2.  **主事件循环**: 管理和响应各种异步事件和计划任务。
3.  **音频处理**: 包括从麦克风读取、Opus 编码发送，以及接收 Opus 数据、解码播放。
4.  **状态管理**: 根据用户交互和系统事件，在不同的设备状态间切换，并执行相应操作。
5.  **OTA 升级与设备激活**: 处理固件更新和设备首次使用的激活流程。
6.  **与协议层交互**: 通过 `Protocol` 接口与后端服务器通信，收发控制指令、音频数据、文本消息等。
7.  **用户界面交互**: 通过 `Display` 模块更新屏幕显示，通过 `Led` 控制指示灯。

**************************************************************************************************************/

#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#else
#include "dummy_audio_processor.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#define TAG "Application"


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
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 8);

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<DummyAudioProcessor>();
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
    esp_timer_start_periodic(clock_timer_handle_, 1000000);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota_.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota_.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

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

        if (ota_.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota_.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
#if CONFIG_USE_WAKE_WORD_DETECT
            wake_word_detect_.StopDetection();
#endif
            // 预先关闭音频输出，避免升级过程有音频操作
            auto codec = board.GetAudioCodec();
            codec->EnableInput(false);
            codec->EnableOutput(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_decode_queue_.clear();
            }
            background_task_->WaitForCompletion();
            delete background_task_;
            background_task_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));

            ota_.StartUpgrade([display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %zuKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            });

            // If upgrade success, the device will reboot and never reach here
            display->SetStatus(Lang::Strings::UPGRADE_FAILED);
            ESP_LOGI(TAG, "Firmware upgrade failed...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            Reboot();
            return;
        }

        // No new version, mark the current version as valid
        ota_.MarkCurrentVersionValid();
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_.HasActivationCode()) {
            ShowActivationCode();
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
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

void Application::ShowActivationCode() {
    auto& message = ota_.GetActivationMessage();
    auto& code = ota_.GetActivationCode();

    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
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

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion();

    // The assets are encoded at 16000Hz, 60ms frame duration
    SetDecodeSampleRate(16000, 60);
    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        AudioStreamPacket packet;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }

            SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop);
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

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    if (realtime_chat_enabled_) {
        ESP_LOGI(TAG, "Realtime chat enabled, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
    } else if (board.GetBoardType() == "ml307") {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
    } else {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 3");
        opus_encoder_->SetComplexity(3);
    }

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, realtime_chat_enabled_ ? 1 : 0);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Check for new firmware version or get the MQTT broker address
    CheckNewVersion();

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        const int max_packets_in_queue = 600 / OPUS_FRAME_DURATION_MS;
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_decode_queue_.size() < max_packets_in_queue) {
            audio_decode_queue_.emplace_back(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        SetDecodeSampleRate(protocol_->server_sample_rate(), protocol_->server_frame_duration());
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
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
                    background_task_->WaitForCompletion();
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
                if (text != NULL) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (text != NULL) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion != NULL) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (commands != NULL) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (command != NULL) {
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
            if (status != NULL && message != NULL && emotion != NULL) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        }
    });
    bool protocol_started = protocol_->Start();

    audio_processor_->Initialize(codec, realtime_chat_enabled_);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            if (protocol_->IsAudioChannelBusy()) {
                return;
            }
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
                packet.timestamp = last_output_timestamp_;
                last_output_timestamp_ = 0;
                Schedule([this, packet = std::move(packet)]() {
                    protocol_->SendAudio(packet);
                });
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });

#if CONFIG_USE_WAKE_WORD_DETECT
    wake_word_detect_.Initialize(codec);
    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (device_state_ == kDeviceStateIdle) {
                SetDeviceState(kDeviceStateConnecting);
                wake_word_detect_.EncodeWakeWordData();

                if (!protocol_ || !protocol_->OpenAudioChannel()) {
                    wake_word_detect_.StartDetection();
                    return;
                }
                
                AudioStreamPacket packet;
                // Encode and send the wake word data to the server
                while (wake_word_detect_.GetWakeWordOpus(packet.payload)) {
                    protocol_->SendAudio(packet);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    wake_word_detect_.StartDetection();
#endif

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);

    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    }
    
    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintRealTimeStats(pdMS_TO_TICKS(1000));

        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (ota_.HasServerTime()) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    if (device_state_ == kDeviceStateListening) {
        audio_decode_queue_.clear();
        audio_decode_cv_.notify_all();
        return;
    }

    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();

    busy_decoding_audio_ = true;
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            return;
        }
        // Resample if the sample rate is different
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        codec->OutputData(pcm);
        last_output_timestamp_ = packet.timestamp;
        last_output_time_ = std::chrono::steady_clock::now();
    });
}

void Application::OnAudioInput() {
#if CONFIG_USE_WAKE_WORD_DETECT
    if (wake_word_detect_.IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_detect_.GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            wake_word_detect_.Feed(data);
            return;
        }
    }
#endif
    if (audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            audio_processor_->Feed(data);
            return;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(30));
}

void Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec->input_sample_rate() != sample_rate) {
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return;
        }
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return;
        }
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
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
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_processor_->Stop();
#if CONFIG_USE_WAKE_WORD_DETECT
            wake_word_detect_.StartDetection();
#endif
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Update the IoT states before sending the start listening command
            UpdateIotStates();

            // Make sure the audio processor is running
            if (!audio_processor_->IsRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                if (listening_mode_ == kListeningModeAutoStop && previous_state == kDeviceStateSpeaking) {
                    // FIXME: Wait for the speaker to empty the buffer
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
                opus_encoder_->ResetState();
#if CONFIG_USE_WAKE_WORD_DETECT
                wake_word_detect_.StopDetection();
#endif
                audio_processor_->Start();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_processor_->Stop();
#if CONFIG_USE_WAKE_WORD_DETECT
                wake_word_detect_.StartDetection();
#endif
            }
            ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();
    
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
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

    // Now it is safe to enter sleep mode
    return true;
}

////////////////////////////////////////

/*
我们来逐段分析：

**1. 头文件包含 (`#include ...`)**

```c++
#include "application.h" // 必须包含自身的头文件
#include "board.h"       // 板级支持包，抽象硬件操作
#include "display.h"     // 显示屏驱动和UI接口
#include "system_info.h" // 系统信息获取 (可能用于调试)
#include "ml307_ssl_transport.h" // 特定硬件(ML307蜂窝模块)的SSL传输层
#include "audio_codec.h" // 音频编解码器驱动接口
#include "mqtt_protocol.h" // MQTT通信协议实现
#include "websocket_protocol.h" // WebSocket通信协议实现
#include "font_awesome_symbols.h" // Font Awesome 图标字体定义
#include "iot/thing_manager.h"   // IoT 物模型管理器
#include "assets/lang_config.h"  // 语言配置和字符串资源

// 条件编译，根据配置选择真实的音频处理器或一个空实现
#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h" // ESP-SR的音频前端处理
#else
#include "dummy_audio_processor.h" // 空的音频处理器，不进行实际处理
#endif

#include <cstring>      // C风格字符串操作
#include <esp_log.h>    // ESP-IDF 日志系统
#include <cJSON.h>      // cJSON库，用于解析和构建JSON数据
#include <driver/gpio.h> // GPIO驱动 (可能用于某些特定控制)
#include <arpa/inet.h>  // 用于网络字节序转换 (如 ntohs)
```

*   包含了项目所需的各种内部模块和外部库。
*   `ml307_ssl_transport.h` 表明支持基于 ML307 模块的蜂窝网络连接。
*   `mqtt_protocol.h` 和 `websocket_protocol.h` 表明支持两种主要的通信协议。
*   `afe_audio_processor.h` (如果启用) 通常指 ESP-SR (Speech Recognition) 库中的音频前端处理，用于语音增强、回声消除等。
*   `lang_config.h` 用于多语言支持。

**2. TAG 定义和状态字符串数组**

```c++
#define TAG "Application" // 日志输出时使用的标签

// 状态枚举对应的字符串表示，方便日志输出和调试
static const char* const STATE_STRINGS[] = {
    "unknown", "starting", "configuring", "idle", "connecting",
    "listening", "speaking", "upgrading", "activating", "fatal_error",
    "invalid_state" // 比枚举多一个，可能是为了数组边界安全
};
```

**3. 构造函数 (`Application::Application()`)**

```c++
Application::Application() {
    // 创建FreeRTOS事件组，用于任务间同步
    event_group_ = xEventGroupCreate();
    // 创建一个后台任务执行器，栈大小为 8 * 4096 bytes
    background_task_ = new BackgroundTask(4096 * 8);

// 根据配置选择实例化 AfeAudioProcessor 或 DummyAudioProcessor
#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<DummyAudioProcessor>();
#endif

    // 配置并创建ESP高精度定时器，用于周期性任务 (OnClockTimer)
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) { // 定时器回调，使用lambda表达式
            Application* app = (Application*)arg;
            app->OnClockTimer();    // 调用成员函数
        },
        .arg = this, // 将当前Application实例指针传递给回调
        .dispatch_method = ESP_TIMER_TASK, // 回调在ESP定时器任务中执行
        .name = "clock_timer",
        .skip_unhandled_events = true // 如果回调处理不过来，跳过未处理的事件
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
    // 启动定时器，周期为1秒 (1000000 微秒)
    esp_timer_start_periodic(clock_timer_handle_, 1000000);
}
```

*   **事件组 (`event_group_`)**: 初始化用于任务间同步和通信的关键组件。
*   **后台任务 (`background_task_`)**: 创建一个 `BackgroundTask` 实例。这个类（未在此处定义，但从其用法推断）可能封装了一个任务或线程池，用于执行一些耗时的操作，避免阻塞主流程或音频处理等实时性要求高的任务。构造函数参数可能是任务栈大小。
*   **音频处理器 (`audio_processor_`)**: 根据 `CONFIG_USE_AUDIO_PROCESSOR` 宏（来自 Kconfig 配置）条件编译，实例化一个真正的音频前端处理器 (`AfeAudioProcessor`) 或一个空壳实现 (`DummyAudioProcessor`)。使用 `std::make_unique` 创建智能指针，符合现代 C++ 风格。
*   **时钟定时器 (`clock_timer_handle_`)**: 创建并启动一个1秒周期的 `esp_timer`。回调函数 `OnClockTimer` 会被周期性调用。注意 lambda 表达式作为回调以及 `this` 指针的传递。

**4. 析构函数 (`Application::~Application()`)**

```c++
Application::~Application() {
    // 停止并删除定时器
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    // 删除后台任务对象
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    // 删除事件组
    vEventGroupDelete(event_group_);
}
```

*   负责释放构造函数中分配的资源，防止内存泄漏和句柄泄漏。

**5. `CheckNewVersion()` 方法**

这个函数负责检查固件更新、执行 OTA 升级（如果需要）以及处理设备激活流程。

```c++
void Application::CheckNewVersion() {
    const int MAX_RETRY = 10; // 最大重试次数
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟10秒

    while (true) { // 循环直到版本检查和激活流程完成或达到最大重试
        SetDeviceState(kDeviceStateActivating); // 设置设备状态为激活中
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION); // 更新显示状态

        if (!ota_.CheckVersion()) { // 调用OTA模块检查版本
            // ... 版本检查失败，带重试逻辑 (指数退避) ...
            // Alert() 显示错误信息，PlaySound() 播放提示音
            // vTaskDelay() 实现延迟
            continue; // 继续下一次重试
        }
        // ... 重置重试计数和延迟 ...

        if (ota_.HasNewVersion()) { // 如果有新版本
            // ... 提示用户即将升级，延时 ...
            SetDeviceState(kDeviceStateUpgrading); // 设置设备状态为升级中
            // ... 更新显示图标和消息 ...
            // ... 关闭节能模式、停止唤醒词、关闭音频输入输出、清空音频解码队列 ...
            // ... 等待后台任务完成并删除，然后重新创建 (FIXME: 这里删除又创建 background_task_ 的逻辑值得商榷，
            //     通常后台任务应该能被暂停或清空任务队列，而不是完全重建) ...
            // 调用 ota_.StartUpgrade() 开始升级，传入一个lambda作为进度回调
            ota_.StartUpgrade([display](int progress, size_t speed) {
                // ... 更新显示升级进度 ...
            });

            // 如果升级成功，设备会重启，不会执行到这里
            // ... 显示升级失败，延时后重启 ...
            return; // 升级失败则退出函数
        }

        // 没有新版本
        ota_.MarkCurrentVersionValid(); // 标记当前版本有效
        // 如果没有激活码且没有激活质询，则表示激活流程已完成或不需要
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT); // 设置事件位，通知主流程
            break; // 退出循环
        }

        display->SetStatus(Lang::Strings::ACTIVATION); // 更新显示为激活状态
        if (ota_.HasActivationCode()) {
            ShowActivationCode(); // 显示激活码给用户 (会播报数字)
        }

        // 循环尝试激活，直到成功或超时
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_.Activate(); // 调用OTA模块执行激活
            if (err == ESP_OK) { // 激活成功
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
                break; // 退出激活尝试循环
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000)); // 超时则延时后重试
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000)); // 其他错误则更长延时后重试
            }
            if (device_state_ == kDeviceStateIdle) { // 如果设备中途变为空闲，也退出
                break;
            }
        }
    } // end while(true)
}
```

*   **健壮性**: 包含重试机制（指数退避策略）来应对网络波动等问题。
*   **用户反馈**: 通过 `Display` 和 `Alert` (包括声音) 向用户提供清晰的状态反馈。
*   **OTA 流程**:
    *   检查新版本。
    *   如果有新版本，准备升级环境（关闭相关服务、音频），然后启动升级。
    *   升级成功设备会重启；失败则提示并重启。
*   **设备激活**:
    *   如果需要激活，会显示激活码（通过 `ShowActivationCode`，会播报数字）。
    *   然后尝试与服务器通信完成激活。
*   **事件同步**: 使用 `CHECK_NEW_VERSION_DONE_EVENT` 事件位通知 `Start()` 函数中的等待逻辑，版本检查和激活流程已完成。

**6. `ShowActivationCode()` 方法**

```c++
void Application::ShowActivationCode() {
    auto& message = ota_.GetActivationMessage(); // 获取激活提示信息
    auto& code = ota_.GetActivationCode();       // 获取激活码字符串

    // 定义数字与其对应提示音的映射
    struct digit_sound { /* ... */ };
    static const std::array<digit_sound, 10> digit_sounds{{ /* ... */ }};

    // 显示激活提示信息，并播放激活提示音
    // 注释提到 "This sentence uses 9KB of SRAM"，说明这个Alert操作可能涉及较多资源
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    // 逐个播报激活码中的数字
    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound); // 播放数字对应的声音
        }
    }
}
```

*   负责向用户展示激活信息和激活码，并通过语音逐个播报激活码数字。

**7. `Alert()` 和 `DismissAlert()` 方法**

```c++
void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion); // 日志记录
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);       // 更新屏幕状态文本
    display->SetEmotion(emotion);     // 更新屏幕表情
    display->SetChatMessage("system", message); // 在聊天区域显示系统消息
    if (!sound.empty()) { // 如果指定了提示音
        ResetDecoder();      // 重置音频解码器状态
        PlaySound(sound);    // 播放提示音
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) { // 仅在空闲状态下清除提示
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}
```

*   `Alert`: 一个通用的提示函数，用于在屏幕上显示状态、消息、表情，并可选播放提示音。
*   `DismissAlert`: 用于清除 `Alert` 显示的内容，恢复到待机界面。

**8. `PlaySound()` 方法**

```c++
void Application::PlaySound(const std::string_view& sound) {
    // 等待上一个声音播放完成
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 使用条件变量等待音频解码队列为空
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion(); // 等待后台任务（如上一个声音的解码）完成

    // 提示音资源被编码为16kHz采样率，60ms帧长
    SetDecodeSampleRate(16000, 60);
    const char* data = sound.data(); // sound 是包含多个音频包的数据块
    size_t size = sound.size();
    // 解析自定义的 BinaryProtocol3 格式的音频包
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p; // 强制类型转换
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size); // 网络字节序转主机字节序
        AudioStreamPacket packet;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        // 将解析出的音频包加入解码队列
        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
}
```

*   负责播放预置在固件中的音效（存储在 `Lang::Sounds` 中，实际数据可能是 `std::string_view` 指向的二进制数据块）。
*   **同步**: 使用互斥锁和条件变量 (`audio_decode_cv_`) 确保上一个声音播放完毕（解码队列为空）才开始播放新的声音。同时等待 `background_task_` 完成，因为音频解码可能在后台任务中进行。
*   **音频格式**: 提示音资源似乎是以一种自定义的 `BinaryProtocol3` 格式打包的，每个包包含 Opus 编码的音频数据。`PlaySound` 解析这些包并将它们放入 `audio_decode_queue_` 等待 `AudioLoop` 中的 `OnAudioOutput` 解码播放。
*   **采样率设置**: 播放前会调用 `SetDecodeSampleRate` 设置 Opus 解码器参数。

**9. 聊天状态控制方法 (`ToggleChatState()`, `StartListening()`, `StopListening()`)**

这些方法处理用户发起或结束对话的逻辑。

```c++
void Application::ToggleChatState() {
    // ... (处理激活状态下的特殊情况) ...
    // ... (检查 protocol_ 是否初始化) ...

    if (device_state_ == kDeviceStateIdle) { // 如果当前是空闲状态
        Schedule([this]() { // 异步调度任务到主事件循环
            SetDeviceState(kDeviceStateConnecting); // 进入连接状态
            if (!protocol_->OpenAudioChannel()) { // 打开音频通道 (如WebSocket连接)
                // 如果打开失败，这里没有明确的错误处理返回到Idle，可能依赖protocol_内部处理或超时
                return;
            }
            // 根据配置设置聆听模式 (实时 或 自动停止)
            SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) { // 如果正在说话 (播放TTS)
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone); // 中断说话
        });
    } else if (device_state_ == kDeviceStateListening) { // 如果正在聆听
        Schedule([this]() {
            protocol_->CloseAudioChannel(); // 关闭音频通道
            // 状态通常会在 OnAudioChannelClosed 回调中设置为 Idle
        });
    }
}

void Application::StartListening() {
    // ... (类似 ToggleChatState 的前置检查和状态处理) ...
    // 主要区别在于，此函数通常用于用户明确要开始说话（如长按按钮）
    // 并将聆听模式设置为手动停止
    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            // ... (打开音频通道逻辑，如果未打开) ...
            SetListeningMode(kListeningModeManualStop); // 设置为手动停止模式
        });
    }
}

void Application::StopListening() {
    // ... (检查当前状态是否允许停止聆听) ...
    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening(); // 通知服务器停止聆听
            SetDeviceState(kDeviceStateIdle); // 直接设置为空闲
        }
    });
}
```

*   **异步调度 (`Schedule`)**: 核心的状态转换和网络操作都通过 `Schedule` 提交到 `MainEventLoop` 执行，避免在中断回调或不合适的任务上下文中执行。
*   **状态驱动**: 根据当前的 `device_state_` 执行不同的操作。
*   **`ToggleChatState`**: 通常由短按按钮或唤醒词触发，在空闲、聆听、说话状态间切换。
*   **`StartListening`**: 可能由长按按钮触发，强制进入聆听状态，并设置为需要手动停止。
*   **`StopListening`**: 明确停止当前的聆听会话。

**10. `Start()` 方法 (核心初始化流程)**

这是应用程序的入口点，在 `app_main` (未显示) 中被调用。

```c++
void Application::Start() {
    auto& board = Board::GetInstance(); // 获取板级对象单例
    SetDeviceState(kDeviceStateStarting); // 设置初始状态为启动中

    /* Setup the display */
    auto display = board.GetDisplay(); // 获取显示对象

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec(); // 获取音频编解码器对象
    // 创建Opus解码器和编码器实例，指定采样率、通道数、帧时长
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    // 根据配置或板型调整Opus编码复杂度 (影响CPU占用和编码质量)
    if (realtime_chat_enabled_) { /* ... complexity 0 ... */ }
    else if (board.GetBoardType() == "ml307") { /* ... complexity 5 ... */ }
    else { /* ... complexity 3 ... */ }

    // 如果编解码器的输入采样率不是16kHz，配置重采样器
    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000); // 参考信号也重采样 (可能用于AEC)
    }
    codec->Start(); // 启动音频编解码器

    // 创建并启动 AudioLoop 任务，固定到CPU核心 (realtime_chat_enabled_ ? 1 : 0)
    xTaskCreatePinnedToCore([](void* arg) { /* ... app->AudioLoop(); ... */ },
        "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, realtime_chat_enabled_ ? 1 : 0);

    /* Wait for the network to be ready */
    board.StartNetwork(); // 启动网络连接 (Wi-Fi 或蜂窝)

    // 检查新版本或获取MQTT代理地址 (这个函数内部会阻塞或异步等待)
    CheckNewVersion();

    // 初始化通信协议
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
    // 根据OTA配置选择MQTT或WebSocket协议
    if (ota_.HasMqttConfig()) { protocol_ = std::make_unique<MqttProtocol>(); }
    else if (ota_.HasWebsocketConfig()) { protocol_ = std::make_unique<WebsocketProtocol>(); }
    else { /* ... 默认MQTT ... */ }

    // 设置协议层的各种事件回调 (使用lambda表达式)
    protocol_->OnNetworkError([this](const std::string& message) { /* ... Alert ... */ });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) { /* ... 加入 audio_decode_queue_ ... */ });
    protocol_->OnAudioChannelOpened([this, codec, &board]() { /* ... 设置解码采样率, 发送IoT描述符和状态 ... */ });
    protocol_->OnAudioChannelClosed([this, &board]() { /* ... 设置节能, 更新UI, 设置Idle状态 ... */ });
    protocol_->OnIncomingJson([this, display](const cJSON* root) { // 处理来自服务器的JSON消息
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) { /* ... 处理TTS开始/结束/文本 ... */ }
        else if (strcmp(type->valuestring, "stt") == 0) { /* ... 处理STT文本 ... */ }
        else if (strcmp(type->valuestring, "llm") == 0) { /* ... 处理LLM情感 ... */ }
        else if (strcmp(type->valuestring, "iot") == 0) { /* ... 处理IoT控制命令 ... */ }
        else if (strcmp(type->valuestring, "system") == 0) { /* ... 处理系统命令 (如reboot) ... */ }
        else if (strcmp(type->valuestring, "alert") == 0) { /* ... 处理服务器推送的Alert ... */ }
    });
    bool protocol_started = protocol_->Start(); // 启动协议栈

    // 初始化音频处理器，并设置其回调
    audio_processor_->Initialize(codec, realtime_chat_enabled_);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) { // 音频处理器输出数据回调 (PCM)
        background_task_->Schedule([this, data = std::move(data)]() mutable { // 异步到后台任务
            // ... (检查音频通道是否繁忙) ...
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) { // Opus编码
                // ... (创建AudioStreamPacket, 设置时间戳, 通过Schedule发送给protocol_) ...
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) { // VAD状态变化回调
        // ... (更新 voice_detected_ 标志, 控制LED状态) ...
    });

#if CONFIG_USE_WAKE_WORD_DETECT // 如果启用了唤醒词
    wake_word_detect_.Initialize(codec); // 初始化唤醒词引擎
    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) { // 唤醒词检测到回调
        Schedule([this, &wake_word]() {
            if (device_state_ == kDeviceStateIdle) {
                // ... (设置连接状态, 编码并发送唤醒前后的音频数据, 发送唤醒词事件, 进入聆听模式) ...
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected); // 如果在说话，则打断
            } // ... (处理激活状态) ...
        });
    });
    wake_word_detect_.StartDetection(); // 开始唤醒词检测
#endif

    // 等待 CheckNewVersion() 流程完成 (通过事件组)
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle); // 设置为空闲状态

    if (protocol_started) { // 如果协议栈启动成功
        // ... (显示版本号, 播放成功提示音) ...
    }
    
    // 进入主事件循环 (此函数会阻塞，直到程序结束)
    MainEventLoop();
}
```

*   **初始化顺序**: `Start()` 方法精心安排了各个组件的初始化顺序：板级 -> 显示 -> 音频编解码与 Opus -> 音频循环任务 -> 网络 -> OTA/激活 -> 通信协议 -> 音频前端处理器 -> 唤醒词。
*   **回调驱动**: 大量使用回调函数（通常是 lambda 表达式）来处理异步事件，如网络数据到达、音频数据就绪、唤醒词检测到等。这是嵌入式和事件驱动编程的典型模式。
*   **JSON 解析**: `OnIncomingJson` 回调使用 `cJSON` 库解析来自服务器的 JSON 消息，根据消息类型 (tts, stt, llm, iot, system, alert) 执行不同操作。
*   **任务创建**: `AudioLoop` 在一个单独的 FreeRTOS 任务中运行，确保音频处理的实时性。任务被固定到特定 CPU核心，可能是为了性能或避免缓存竞争。
*   **同步**: `xEventGroupWaitBits` 用于等待 `CheckNewVersion` 异步流程的完成。

**11. `OnClockTimer()` 方法**

```c++
void Application::OnClockTimer() {
    clock_ticks_++; // 时钟滴答计数

    if (clock_ticks_ % 10 == 0) { // 每10秒执行一次
        // ... (打印实时统计信息，如内存占用) ...
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

        if (ota_.HasServerTime()) { // 如果已同步到服务器时间
            if (device_state_ == kDeviceStateIdle) { // 且设备空闲
                Schedule([this]() { // 异步更新显示
                    // 将状态栏设置为当前时间 "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}
```

*   由构造函数中创建的 `esp_timer` 每秒调用一次。
*   用于执行周期性任务，例如：
    *   每10秒打印一次内存使用情况等调试信息。
    *   如果设备空闲且已获取服务器时间，则在屏幕上显示当前时间。

**12. `Schedule()` 和 `MainEventLoop()` 方法**

```c++
void Application::Schedule(std::function<void()> callback) {
    { // 锁保护 main_tasks_ 列表
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback)); // 将回调函数加入任务列表
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT); // 设置事件位，唤醒 MainEventLoop
}

void Application::MainEventLoop() {
    while (true) {
        // 等待 SCHEDULE_EVENT 事件位 (清除已处理的事件位，不等待所有位，永久等待)
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SCHEDULE_EVENT) { // 如果是 SCHEDULE_EVENT 触发
            std::unique_lock<std::mutex> lock(mutex_); // 加锁
            std::list<std::function<void()>> tasks = std::move(main_tasks_); // 取出当前所有任务
            lock.unlock(); // 尽快解锁，允许其他地方继续 Schedule
            for (auto& task : tasks) {
                task(); // 逐个执行任务列表中的回调函数
            }
        }
    }
}
```

*   **`Schedule(callback)`**: 提供了一个机制，允许其他任务或回调将一个函数（封装在 `std::function` 中）提交给 `MainEventLoop` 来执行。这是为了确保某些操作（尤其是涉及共享资源或状态改变的操作）都在同一个任务上下文（`MainEventLoop` 所在的任务）中顺序执行，避免复杂的线程同步问题。
*   **`MainEventLoop()`**: 应用的主事件循环。它不断等待 `SCHEDULE_EVENT` 事件。一旦事件发生，它会取出 `main_tasks_` 列表中的所有待处理任务并依次执行。这是典型的生产者-消费者模式，`Schedule` 是生产者，`MainEventLoop` 是消费者。

**13. 音频处理循环 (`AudioLoop()`, `OnAudioInput()`, `OnAudioOutput()`, `ReadAudio()`)**

```c++
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) { // 无限循环处理音频
        OnAudioInput(); // 处理音频输入
        if (codec->output_enabled()) { // 如果音频输出是使能的
            OnAudioOutput(); // 处理音频输出
        }
        // 注意：这里没有 vTaskDelay，意味着 AudioLoop 会尽可能快地运行。
        // OnAudioInput 和 OnAudioOutput 内部需要有阻塞或延时机制，
        // 否则这个任务会持续占用CPU。
        // OnAudioInput 内部有 vTaskDelay(pdMS_TO_TICKS(30)) 如果没有数据可读。
        // OnAudioOutput 内部如果没有数据，会直接返回。
    }
}

void Application::OnAudioOutput() { // 处理音频播放
    if (busy_decoding_audio_) { return; } // 如果正在解码，则本次跳过，避免重入或过多后台任务

    // ... (检查解码队列是否为空，如果空闲且长时间无数据则关闭codec输出) ...

    if (device_state_ == kDeviceStateListening) { // 如果在聆听状态，清空播放队列（不播放自己的声音）
        audio_decode_queue_.clear();
        audio_decode_cv_.notify_all(); // 通知 PlaySound 可能在等待队列为空
        return;
    }

    // 从解码队列取出一个音频包
    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock(); // 解锁后尽快通知
    audio_decode_cv_.notify_all(); // 通知 PlaySound 队列已取走一个包

    busy_decoding_audio_ = true; // 标记开始解码
    // 将解码任务调度到后台任务执行器
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false; // 解码完成或开始前重置标志
        if (aborted_) { return; } // 如果已被中断，则不解码

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) { // 调用Opus解码
            return; // 解码失败
        }
        // 如果解码后的采样率与codec输出采样率不同，则进行重采样
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            // ... (使用 output_resampler_ 进行重采样) ...
        }
        codec->OutputData(pcm); // 将PCM数据发送给音频codec播放
        last_output_timestamp_ = packet.timestamp; // 记录最后播放包的时间戳
        last_output_time_ = std::chrono::steady_clock::now(); // 记录当前时间
    });
}

void Application::OnAudioInput() { // 处理音频录制
#if CONFIG_USE_WAKE_WORD_DETECT
    if (wake_word_detect_.IsDetectionRunning()) { // 如果唤醒词检测正在运行
        std::vector<int16_t> data;
        int samples = wake_word_detect_.GetFeedSize(); // 获取唤醒引擎需要的样本数
        if (samples > 0) {
            ReadAudio(data, 16000, samples); // 读取指定数量和采样率的音频
            wake_word_detect_.Feed(data);    // 将数据喂给唤醒引擎
            return; // 本次输入处理完毕
        }
    }
#endif
    if (audio_processor_->IsRunning()) { // 如果音频前端处理器正在运行 (通常在聆听状态)
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize(); // 获取处理器需要的样本数
        if (samples > 0) {
            ReadAudio(data, 16000, samples); // 读取音频
            audio_processor_->Feed(data);    // 将数据喂给音频处理器
            return; // 本次输入处理完毕
        }
    }

    vTaskDelay(pdMS_TO_TICKS(30)); // 如果上面都没有读取数据，则延时30ms再试
}

void Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    // 如果codec的原始采样率与目标采样率不同
    if (codec->input_sample_rate() != sample_rate) {
        // 根据采样率比例调整读取大小，然后读取原始数据
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) { return; } // 从codec读取数据
        // 如果是双声道 (通常一个是mic, 一个是参考信号AEC)
        if (codec->input_channels() == 2) {
            // ... (分离左右声道，分别用 input_resampler_ 和 reference_resampler_ 进行重采样，
            //      然后再合并回 data。这部分逻辑比较复杂，涉及到声道分离、重采样、合并) ...
        } else { // 单声道
            // ... (使用 input_resampler_ 进行重采样) ...
        }
    } else { // 采样率相同，直接读取
        data.resize(samples);
        if (!codec->InputData(data)) { return; }
    }
}
```

*   **`AudioLoop` 任务**: 一个高优先级的任务，不断调用 `OnAudioInput` 和 `OnAudioOutput`。
*   **`OnAudioInput`**:
    *   优先将音频数据喂给唤醒词引擎（如果正在运行）。
    *   否则，如果音频前端处理器正在运行（通常在聆听状态），则将数据喂给它。
    *   如果两者都不需要数据，则短暂延时。
*   **`ReadAudio`**: 封装了从 `AudioCodec` 读取原始音频数据，并根据需要进行重采样（如果 Codec 采样率与目标采样率不同）和声道处理的逻辑。这里对双声道的处理（分离、分别重采样、合并）表明可能支持参考信号用于回声消除 (AEC)。
*   **`OnAudioOutput`**:
    *   从 `audio_decode_queue_` 中取出 Opus 包。
    *   使用 `background_task_` 将解码操作（`opus_decoder_->Decode()`）异步化，避免阻塞 `AudioLoop`。
    *   解码后的 PCM 数据如果采样率不匹配，会进行重采样。
    *   最终将 PCM 数据交给 `AudioCodec` 播放。
    *   `busy_decoding_audio_` 标志用于防止同时调度多个解码任务。
    *   在聆听状态下会清空播放队列，避免播放用户自己的声音。

**14. 其他辅助方法**

*   **`AbortSpeaking(AbortReason reason)`**: 中断当前 TTS 播放，并通知服务器。
*   **`SetListeningMode(ListeningMode mode)`**: 设置聆听模式（自动停止、手动停止、实时）并进入聆听状态。
*   **`SetDeviceState(DeviceState state)`**: 核心的状态转换函数。
    *   记录日志、等待后台任务完成。
    *   根据新的状态更新 `Display` 和 `Led`。
    *   执行进入新状态所需的操作（如启动/停止音频处理器、唤醒词检测、发送网络命令）。
*   **`ResetDecoder()`**: 重置 Opus 解码器状态，清空解码队列，使能音频输出。
*   **`SetDecodeSampleRate(int sample_rate, int frame_duration)`**: 重新配置 Opus 解码器以适应不同的输入音频流参数。如果参数变化，会销毁并重建解码器实例。
*   **`UpdateIotStates()`**: 获取最新的 IoT 设备状态并发送给服务器。
*   **`Reboot()`**: 重启设备。
*   **`WakeWordInvoke(const std::string& wake_word)`**: 这是 `.h` 文件中声明的公共接口，但在这个 `.cc` 文件的实现中，它似乎与 `#if CONFIG_USE_WAKE_WORD_DETECT` 块内的 `wake_word_detect_.OnWakeWordDetected` 回调逻辑有些重叠或不同步。
    *   `.h` 中的 `WakeWordInvoke` 是一个独立的公有方法。
    *   `.cc` 中 `wake_word_detect_.OnWakeWordDetected` 回调是实际处理唤醒词检测结果的地方。
    *   `WakeWordInvoke` 的实现更像是对标 `ToggleChatState`，根据当前状态做不同处理，并且会尝试发送 `SendWakeWordDetected`。而 `OnWakeWordDetected` 回调则包含了更完整的唤醒后音频数据处理和发送逻辑。**这两者之间的确切关系和调用流程需要仔细梳理，可能存在设计上的演进或特定场景的区分。**
*   **`CanEnterSleepMode()`**: 判断设备当前是否可以安全进入睡眠模式的条件。

**总结 `application.cc`**

*   **高度事件驱动和状态驱动**: 整个应用围绕 `DeviceState` 状态机和 `MainEventLoop` 的事件处理构建。
*   **模块化和抽象**: 通过 `Board`, `AudioCodec`, `Protocol`, `Display`, `AudioProcessor` 等接口与具体实现解耦。
*   **并发处理**: 使用 FreeRTOS 任务 (`AudioLoop`, `background_task_`) 和同步原语 (`mutex_`, `event_group_`, `condition_variable`) 处理并发操作。
*   **资源管理**: `std::unique_ptr` 用于管理动态对象，析构函数负责释放系统资源。
*   **回调机制**: 广泛使用 lambda 表达式作为回调，实现异步操作的响应。
*   **全面的功能**: 实现了语音交互的核心链路（唤醒、录音、编码、网络传输、接收、解码、播放）、OTA、设备激活、状态显示、IoT 控制等。
*   **健壮性考虑**: 包含重试、错误处理、超时机制。
*   **可配置性**: 通过 Kconfig 宏（如 `CONFIG_USE_AUDIO_PROCESSOR`, `CONFIG_USE_WAKE_WORD_DETECT`）实现功能的条件编译。

这份代码展示了一个功能相对完整且设计较为精良的嵌入式语音助手应用。
理解它的关键在于跟踪 `Start()` 的初始化流程，以及 `MainEventLoop` 和 `AudioLoop` 这两个核心任务如何与各种回调函数和状态转换协同工作。
*/
