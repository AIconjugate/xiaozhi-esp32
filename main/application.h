// 这个头文件定义了 `Application` 类，它是整个小智 ESP32 项目的核心控制器。

#ifndef _APPLICATION_H_ // 1. 头文件保护，防止重复包含 (全程)
#define _APPLICATION_H_

// 2. FreeRTOS 相关头文件
#include <freertos/FreeRTOS.h>     // FreeRTOS核心功能
#include <freertos/event_groups.h> // FreeRTOS事件组，用于任务间同步和通信
#include <freertos/task.h>         // FreeRTOS任务管理

// 3. ESP-IDF 系统服务头文件
#include <esp_timer.h>             // ESP32高精度定时器

// 4. C++ 标准库头文件
#include <string>                  // 字符串处理
#include <mutex>                   // 互斥锁，用于多线程同步
#include <list>                    // 链表容器
#include <vector>                  // 动态数组容器
#include <condition_variable>      // 条件变量，用于多线程同步
#include <memory>                  // 智能指针 (如 std::unique_ptr)

// 5. Opus 音频编解码及重采样相关头文件 (比如播放P3?)
#include <opus_encoder.h>          // Opus编码器
#include <opus_decoder.h>          // Opus解码器
#include <opus_resampler.h>        // Opus重采样器 (用于转换音频采样率)

// 6. 项目内部模块头文件
#include "protocol.h"             // 通信协议模块 (如WebSocket, UDP)
#include "ota.h"                  // OTA (Over-The-Air) 固件升级模块
#include "background_task.h"      // 后台任务封装
#include "audio_processor.h"      // 音频处理模块 (可能包含ADC/DAC驱动、音频流管理等)

// 7. 条件编译：如果启用了唤醒词检测功能
// 使用唤醒词: 提供了“永远在线”（Always-On）的语音交互入口。用户可以直接通过说出唤醒词（如“小智小智”）来激活设备并开始对话，无需物理接触，提供了便利性。对于智能音箱、智能家居中控等很重要。
// 不使用唤醒词: 设备通常需要通过物理按键、触摸、手机 App 指令或其他传感器（如 PIR 人体感应）来激活。交互的即时性和便捷性会降低。
#if CONFIG_USE_WAKE_WORD_DETECT
    #include "wake_word_detect.h"     // 唤醒词检测模块
    // 这里还可以定义一个 realtime_chat_enabled_ 指定实时聊天
#endif

// 8. 事件组位定义
// 这些宏定义了事件组中的不同事件位，用于在任务间传递信号
#define SCHEDULE_EVENT (1 << 0)               // 调度事件标志 (可能有任务需要在主循环中执行)
#define AUDIO_INPUT_READY_EVENT (1 << 1)      // 音频输入数据准备好事件标志
#define AUDIO_OUTPUT_READY_EVENT (1 << 2)     // 音频输出数据准备好事件标志 (可能指TTS数据已解码)
#define CHECK_NEW_VERSION_DONE_EVENT (1 << 3) // 检查新版本完成事件标志

// 9. 设备状态枚举
// 定义了设备可能处于的各种状态，构成一个状态机
enum DeviceState {
    kDeviceStateUnknown,         // 未知状态
    kDeviceStateStarting,        // 启动中
    kDeviceStateWifiConfiguring, // Wi-Fi配置中
    kDeviceStateIdle,            // 空闲状态，等待唤醒
    kDeviceStateConnecting,      // 连接中 (连接到服务器)
    kDeviceStateListening,       // 聆听中 (正在录音并发送语音)
    kDeviceStateSpeaking,        // 说话中 (正在播放TTS语音)
    kDeviceStateUpgrading,       // 升级中 (OTA)
    kDeviceStateActivating,      // 激活中 (设备首次使用或重新激活)
    kDeviceStateFatalError       // 严重错误状态
};

// 10. Opus音频帧时长定义 (毫秒; 60ms是最长的帧长选项, 适用于对音质要求较高 & 可接受稍高延迟的场景)
#define OPUS_FRAME_DURATION_MS 60

// 11. Application 类声明
class Application {
public:
    // 11.1 单例模式实现：提供全局唯一的Application实例访问点
    static Application& GetInstance() {
        static Application instance; // C++11及以后版本的线程安全的局部静态变量初始化
        return instance;
    }

    // 11.2 删除拷贝构造函数和赋值运算符，防止意外复制单例对象
    Application(const Application&) = delete; // 拷贝构造函数 (Copy Constructor) 的声明, `= delete` 表示**“删除”这个函数**
    Application& operator=(const Application&) = delete; // 拷贝赋值运算符 (Copy Assignment Operator) 的声明类似

    // 11.3 公共接口方法
    void Start();                                           // 启动应用程序的核心逻辑
    DeviceState GetDeviceState() const { return device_state_; } // 获取当前设备状态
    bool IsVoiceDetected() const { return voice_detected_; }    // 查询是否检测到语音活动
    void Schedule(std::function<void()> callback);          // 调度一个回调函数在主事件循环中执行
    void SetDeviceState(DeviceState state);                 // 设置设备状态 (可能会触发UI更新、行为改变等)

    // 显示提示信息 (状态、消息内容、可选情绪、可选提示音)
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();                                    // 清除提示信息
    void AbortSpeaking(AbortReason reason);                 // 中断当前的TTS播放 (AbortReason枚举未在此处定义，但应在别处)
    void ToggleChatState();                                 // 切换聊天状态 (例如：从空闲到聆听，或从聆听到处理)
    void StartListening();                                  // 开始聆听用户语音
    void StopListening();                                   // 停止聆听用户语音
    void UpdateIotStates();                                 // 更新IoT设备状态 (如果集成了物联网控制)
    void Reboot();                                          // 重启设备
    void WakeWordInvoke(const std::string& wake_word);      // 唤醒词被触发时的回调处理
    void PlaySound(const std::string_view& sound);          // 播放指定的音效 (std::string_view用于高效传递字符串)
    bool CanEnterSleepMode();                               // 判断设备是否可以进入睡眠模式

private:
    // 11.4 私有构造函数和析构函数 (单例模式的一部分)
    Application();
    ~Application();

    // 11.5 私有成员变量 - 对象实例
#if CONFIG_USE_WAKE_WORD_DETECT
    WakeWordDetect wake_word_detect_; // 唤醒词检测对象 (条件编译)
#endif
    std::unique_ptr<AudioProcessor> audio_processor_; // 音频处理器对象 (使用智能指针管理生命周期)
    Ota ota_;                                         // OTA升级处理对象

    std::unique_ptr<Protocol> protocol_;              // 通信协议处理对象 (使用智能指针)
    BackgroundTask* background_task_ = nullptr;       // 指向后台任务对象的指针

    // 11.6 私有成员变量 - 同步与任务调度
    std::mutex mutex_;                                  // 互斥锁，保护共享资源
    std::list<std::function<void()>> main_tasks_;       // 存储通过Schedule()方法提交的任务列表
    EventGroupHandle_t event_group_ = nullptr;          // FreeRTOS事件组句柄
    esp_timer_handle_t clock_timer_handle_ = nullptr;   // ESP高精度定时器句柄 (可能用于周期性任务)
    TaskHandle_t check_new_version_task_handle_ = nullptr; // 检查新版本任务的句柄
    TaskHandle_t audio_loop_task_handle_ = nullptr;     // 音频处理循环任务的句柄

    // 11.7 私有成员变量 - 状态与标志
    volatile DeviceState device_state_ = kDeviceStateUnknown; // 当前设备状态 (volatile修饰，可能被中断或不同任务修改)
    ListeningMode listening_mode_ = kListeningModeAutoStop;   // 聆听模式 (ListeningMode枚举未在此处定义)
#if CONFIG_USE_REALTIME_CHAT
    bool realtime_chat_enabled_ = true;                 // 是否启用实时聊天 (条件编译)
#else
    bool realtime_chat_enabled_ = false;
#endif
    bool aborted_ = false;                              // 标记TTS播放是否被中断
    bool voice_detected_ = false;                       // 标记是否检测到语音活动
    bool busy_decoding_audio_ = false;                  // 标记是否正在忙于解码音频

    // 11.8 私有成员变量 - 计时与时间戳
    int clock_ticks_ = 0;                               // 时钟滴答计数
    std::chrono::steady_clock::time_point last_output_time_; // 上次音频输出的时间点 (用于超时等判断)
    std::atomic<uint32_t> last_output_timestamp_ = 0;   // 上次音频输出的时间戳 (原子操作，线程安全)

    // 11.9 私有成员变量 - 音频解码队列与同步
    std::list<AudioStreamPacket> audio_decode_queue_;   // 音频解码队列 (AudioStreamPacket结构体/类未在此处定义)
    std::condition_variable audio_decode_cv_;           // 条件变量，用于通知音频解码队列有新数据

    // 11.10 私有成员变量 - Opus编解码器及重采样器实例
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_; // Opus编码器封装对象 (智能指针)
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_; // Opus解码器封装对象 (智能指针)

    OpusResampler input_resampler_;                     // 输入音频重采样器
    OpusResampler reference_resampler_;                 // 参考音频重采样器 (可能用于回声消除等高级功能)
    OpusResampler output_resampler_;                    // 输出音频重采样器

    // 11.11 私有方法声明 - 内部逻辑实现
    void MainEventLoop();                               // 主事件循环，处理应用的核心逻辑和事件
    void OnAudioInput();                                // 处理音频输入事件的回调
    void OnAudioOutput();                               // 处理音频输出事件的回调 (可能用于播放TTS)
    void ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples); // 读取音频数据
    void ResetDecoder();                                // 重置Opus解码器
    void SetDecodeSampleRate(int sample_rate, int frame_duration); // 设置Opus解码器的采样率和帧时长
    void CheckNewVersion();                             // 检查固件新版本
    void ShowActivationCode();                          // 显示激活码 (用于设备激活流程)
    void OnClockTimer();                                // 定时器回调函数
    void SetListeningMode(ListeningMode mode);          // 设置聆听模式
    void AudioLoop();                                   // 音频处理的主循环任务函数
};

#endif // _APPLICATION_H_

/////////////////////////////

/*
1.  **头文件保护 (`#ifndef`, `#define`, `#endif`)**: 防止同一个头文件在编译过程中被多次包含，避免重复定义错误。
2.  **FreeRTOS**: 表明应用基于 FreeRTOS 实时操作系统，利用其任务管理 (`Task.h`) 和事件组 (`event_groups.h`) 功能进行并发控制和任务间通信。
3.  **`esp_timer.h`**: 使用 ESP32 的高精度定时器，可能用于实现自定义的周期性任务或超时检测。
4.  **C++ 标准库**:
    *   `string`, `list`, `vector`: 用于数据存储和管理。
    *   `mutex`, `condition_variable`: 用于在多任务环境下保护共享资源和实现线程同步，这对于音频处理、网络通信等并发操作至关重要。
    *   `memory` (`std::unique_ptr`): 用于自动管理动态分配对象的生命周期，防止内存泄漏，是现代 C++ 的良好实践。
5.  **Opus 音频**: 表明项目使用 Opus 编解码器。Opus 是一种高效的开源音频编解码格式，常用于网络语音通信。
    *   `opus_encoder.h`: 将采集到的 PCM 音频数据编码为 Opus 格式，以减小数据量，方便网络传输。
    *   `opus_decoder.h`: 将从服务器接收到的 Opus 音频数据解码为 PCM 数据，以便播放。
    *   `opus_resampler.h`: 用于在不同采样率的音频数据间进行转换，例如麦克风的采样率可能与 Opus 编码或 TTS 输出要求的采样率不同。
6.  **项目内部模块**:
    *   `protocol.h`: 抽象了与后端服务器的通信协议，可能是 WebSocket 或 UDP。`Application` 类会通过这个模块发送语音数据、接收 LLM 回复和 TTS 音频。
    *   `ota.h`: 实现了固件的在线升级功能。
    *   `background_task.h`: 可能是一个通用的后台任务执行框架或帮助类。
    *   `audio_processor.h`: 封装了更底层的音频处理逻辑，如从 ADC 读取数据、向 DAC 写入数据、音频流的缓冲和管理等。
7.  **条件编译 (`#if CONFIG_USE_WAKE_WORD_DETECT`)**: 允许在编译时根据配置选择是否包含唤醒词检测功能。这使得固件可以根据具体硬件或需求进行定制。
8.  **事件组位定义**: `EventGroupHandle_t event_group_` 会使用这些位。例如，当音频输入模块准备好数据后，会设置 `AUDIO_INPUT_READY_EVENT` 位，而 `MainEventLoop` 或其他任务可以等待这个事件位。
9.  **`DeviceState` 枚举**: 这是应用状态机的核心。`Application` 类会根据外部事件（如用户输入、网络变化）和内部逻辑在这些状态间切换，并执行相应的操作。
10. **`OPUS_FRAME_DURATION_MS`**: 定义了 Opus 编码时每一帧音频的时长，通常 Opus 支持 2.5ms 到 120ms 的帧长，60ms 是一个常见选择。
11. **`Application` 类**:
    *   **单例模式**: `GetInstance()` 确保全局只有一个 `Application` 实例，方便各模块访问核心控制逻辑。删除拷贝构造和赋值运算符是实现单例的必要步骤。
    *   **公共接口 (`public`)**:
        *   `Start()`: 初始化所有子模块、创建任务、进入主事件循环。
        *   `GetDeviceState()`, `SetDeviceState()`: 状态机的读取和控制。
        *   `Schedule()`: 提供了一种将函数（任务）提交到 `Application` 主线程（很可能是 `MainEventLoop` 运行的线程）执行的机制，避免了直接在回调或其他线程中执行复杂或需要同步的操作。使用 `std::function` 增加了灵活性。
        *   `Alert()`, `DismissAlert()`: 用于向用户显示状态或错误信息，可能通过屏幕或指示灯。
        *   `AbortSpeaking()`: 允许打断当前的语音播报。
        *   `ToggleChatState()`, `StartListening()`, `StopListening()`: 控制对话流程的核心方法。
        *   `WakeWordInvoke()`: 当 `wake_word_detect_` 模块检测到唤醒词时，会调用此方法来启动交互。
        *   `PlaySound()`: 播放预定义的提示音或音效。
        *   `CanEnterSleepMode()`: 用于电源管理。
    *   **私有构造/析构 (`private Application()`, `~Application()`)**: 配合单例模式。
    *   **私有成员变量**:
        *   对象实例 (`wake_word_detect_`, `audio_processor_`, `ota_`, `protocol_`, `background_task_`): 持有各个功能模块的实例。`std::unique_ptr` 的使用表明了对资源管理的重视。
        *   同步与任务调度 (`mutex_`, `main_tasks_`, `event_group_`, `clock_timer_handle_`, ...`_task_handle_`): FreeRTOS 和 C++ 标准库的同步原语，用于协调并发任务。
        *   状态与标志 (`device_state_`, `listening_mode_`, `realtime_chat_enabled_`, `aborted_`, `voice_detected_`, `busy_decoding_audio_`): 存储应用的内部状态和各种标志位。`volatile` 修饰 `device_state_` 是因为它可能在中断服务程序或不同的任务中被修改和访问。
        *   计时与时间戳 (`clock_ticks_`, `last_output_time_`, `last_output_timestamp_`): 用于实现超时、节拍计数等功能。`std::atomic` 保证了时间戳在多线程环境下的原子性访问。
        *   音频解码队列 (`audio_decode_queue_`, `audio_decode_cv_`): 当从网络接收到 Opus 音频包时，会放入此队列，`AudioLoop` 任务会从中取出数据进行解码和播放。`std::condition_variable` 用于在队列为空时阻塞消费者任务，在有新数据时唤醒它。
        *   Opus 编解码器及重采样器实例 (`opus_encoder_`, `opus_decoder_`, `..._resampler_`): 实际执行音频编解码和采样率转换的对象。`Opus...Wrapper` 暗示这些可能是对原生 Opus C API 的 C++ 封装，以提供更易用的接口。
    *   **私有方法 (`private`)**:
        *   `MainEventLoop()`: 应用的主循环，会等待并处理来自 `event_group_` 的事件，或者处理 `main_tasks_` 队列中的任务。
        *   `OnAudioInput()`, `OnAudioOutput()`: 很可能是由 `audio_processor_` 或相关中断调用的回调，用于处理原始音频数据的流入和流出。
        *   `ReadAudio()`: 具体的音频读取实现。
        *   `ResetDecoder()`, `SetDecodeSampleRate()`: 控制 Opus 解码器的内部状态。
        *   `CheckNewVersion()`: OTA 检查的具体逻辑，可能在一个单独的任务 (`check_new_version_task_handle_`) 中运行。
        *   `ShowActivationCode()`: 设备激活流程的一部分。
        *   `OnClockTimer()`: 由 `clock_timer_handle_` 定期调用的函数，可以执行周期性检查或更新。
        *   `SetListeningMode()`: 修改聆听行为的内部方法。
        *   `AudioLoop()`: 一个专门的任务函数，负责从 `audio_decode_queue_` 取数据、解码 Opus、可能还包括将 PCM 数据发送给 `audio_processor_` 进行播放。也可能负责从 `audio_processor_` 获取麦克风数据、编码 Opus 并通过 `protocol_` 发送。

**总结来说，`application.h` 定义了一个高度结构化、事件驱动、基于状态机的应用核心。它：**

*   **采用单例模式**进行全局控制。
*   **深度集成 FreeRTOS** 进行任务管理和同步。
*   **大量使用 C++ 特性**，如智能指针、STL 容器、同步原语，以实现健壮和高效的代码。
*   **封装了核心功能模块**，如音频处理 (包括 Opus 编解码)、网络通信、OTA 升级、唤醒词检测。
*   **通过事件组和回调函数**实现了模块间的解耦和异步通信。
*   **定义了清晰的设备状态**，并围绕状态机设计了核心交互逻辑。

这个头文件展示了一个复杂嵌入式 AI 应用的良好架构基础。
*/
