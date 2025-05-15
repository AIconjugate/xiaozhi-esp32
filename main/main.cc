#include <esp_log.h>      // ESP-IDF 日志打印功能
#include <esp_err.h>      // ESP-IDF 错误代码定义
#include <nvs.h>          // NVS (Non-Volatile Storage) 非易失性存储库，用于存储Wi-Fi凭据、配置等
#include <nvs_flash.h>    // NVS Flash分区初始化功能
#include <driver/gpio.h>  // GPIO (General Purpose Input/Output)驱动，用于控制硬件引脚
#include <esp_event.h>    // ESP-IDF 事件循环库，用于处理系统事件（如Wi-Fi连接、按钮按下等）

#include "application.h"  // 关键：引入了自定义的 "application.h" 头文件，这通常是应用层逻辑的封装
#include "system_info.h"  // 引入了自定义的 "system_info.h" 头文件，用于获取或管理系统信息

#define TAG "main"        // 定义一个日志标签，方便在日志输出中识别来源


extern "C" void app_main(void) // ESP-IDF 应用的主入口函数，extern "C" 是为了C++代码能被C语言的启动代码正确调用
{
    // Initialize the default event loop
    // 初始化默认事件循环。ESP-IDF中很多操作是事件驱动的，比如网络连接、GPIO中断等。
    // 这个事件循环是处理这些异步事件的基础。
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    // 初始化NVS Flash。NVS用于持久化存储数据，即使设备重启也不会丢失。
    // 项目中常用来保存Wi-Fi账号密码、用户设置等。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 如果NVS分区损坏或版本不兼容，则擦除并重新初始化。这是一种容错机制。
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); // 确保NVS初始化成功

    // Launch the application 启动应用程序的核心逻辑。
    // Application::GetInstance() 表明 Application 是一个单例类 (Singleton pattern)。
    // 这意味着整个应用中只有一个 Application 实例。
    // .Start() 调用了这个单例的 Start 方法，应用的具体业务逻辑应该从这里开始执行。
    Application::GetInstance().Start();
}

/*

**总结 `main.cc` 的作用：**

1.  **基础服务初始化**：初始化了 ESP-IDF 的核心功能，如事件循环和非易失性存储 (NVS)。这些是任何复杂 ESP32 应用所必需的。
2.  **错误处理**：对 NVS 初始化可能出现的错误进行了处理，保证了系统的健壮性。
3.  **启动应用核心**：最重要的一步是调用 `Application::GetInstance().Start()`。
    这表明 `main.cc` 本身并不包含复杂的业务逻辑，而是将控制权交给了 `Application` 类。
    这是一种良好的设计模式，使得 `main` 函数保持简洁，而将复杂的应用逻辑封装在专门的类中。
4. 接下来可以 **阅读 `application.h` 和 `application.cc`**：这是理解项目核心逻辑的关键。看 `Application` 类是如何组织和调用其他模块的。
5. 也可以 **跟踪一个核心功能**：例如，从按键唤醒开始，跟踪代码是如何一步步采集音频、进行语音识别、发送到云端 LLM、接收回复、然后通过 TTS 播放出来的。这将串联起各个模块。

总而言之，`main.cc` 只是一个起点，真正的“魔法”发生在 `Application` 类以及它所调用的各种功能模块中。这个项目看起来采用了比较现代和模块化的 C++ 设计方法。
虽然初看可能复杂，但理解了其分层和模块化的思想后，会更容易上手。

-------

**推测工程结构**

基于 `main.cc` 的内容和 GitHub 页面的信息，我们可以对小智 ESP32 项目的整体结构做出如下推测：

1.  **核心应用层 (`Application` 类)**：
    *   `application.h` 和对应的 `application.cc` (或 `.cpp`) 文件会定义和实现 `Application` 类。
    *   这个类是整个项目的**总指挥**。它的 `Start()` 方法内部会进一步初始化和管理项目所需的各个模块。
    *   它可能会包含一个主循环或者状态机来管理设备的不同状态（如等待唤醒、录音、网络请求、播放声音、显示信息等）。

2.  **模块化设计**：
    *   从 GitHub 页面列出的“已实现功能”来看，项目会包含很多独立的模块。这些模块很可能被封装成不同的类或文件集合，由 `Application` 类来协调。例如：
        *   **网络管理模块**：处理 Wi-Fi 连接、4G (ML307) 连接，以及 WebSocket 或 UDP 通信。
        *   **音频输入模块**：处理麦克风数据采集、离线语音唤醒 (ESP-SR)、流式语音识别 (SenseVoice)。
        *   **音频输出模块**：处理 TTS 播放 (火山引擎/CosyVoice)，可能通过 I2S 驱动扬声器。
        *   **LLM 交互模块**：负责与大语言模型 (Qwen, DeepSeek, Doubao) 的 API 进行通信，发送用户语音转换的文本，接收模型返回的文本。
        *   **硬件驱动模块**：
            *   **按键处理**：处理 BOOT 键的单击和长按。
            *   **显示屏驱动**：控制 OLED 或 LCD 显示屏，显示文本、信号强度、图片表情。
        *   **配置管理模块**：可能用于加载和应用 `xiaozhi.me` 控制台上的配置（如提示词、音色）。
        *   **内存管理/状态管理模块**：实现短期记忆，对话总结等。
        *   **多语言支持模块**：管理不同语言的识别和 TTS。
        *   **声纹识别模块**：集成 SenseVoice 的声纹识别功能。

3.  **目录结构 (基于 GitHub 页面信息)**：
    *   `main/`: 这个目录通常存放项目的主要源代码。`main.cc` 就在这里面。`application.h`, `application.cc` 以及上述推测的各个功能模块的源文件和头文件也应该在这里或其子目录中。
    *   `.github/`: 存放 GitHub 相关配置文件，如 workflows (CI/CD 自动化脚本)。
    *   `docs/`: 存放项目文档。
    *   `scripts/`: 存放一些辅助脚本（如编译、烧录、工具脚本等）。
    *   根目录下的文件：
        *   `CMakeLists.txt`: ESP-IDF 项目的构建配置文件。
        *   `LICENSE`: 项目的许可证文件 (MIT)。
        *   `README.md` (及其他语言版本): 项目说明文档。
        *   `partitions.csv` (及不同 Flash 大小的版本): 定义 ESP32 Flash 分区表，指定不同功能的固件存储区域。
        *   `sdkconfig.defaults` (及不同芯片型号的版本): ESP-IDF 项目的默认配置选项。

4.  **第三方库和 SDK**：
    *   **ESP-IDF**: 核心开发框架。
    *   **ESP-SR**: 乐鑫官方的语音识别库，用于离线唤醒等。
    *   **SenseVoice**: 用于流式语音识别和声纹识别。
    *   可能还有其他用于解析 JSON、HTTP 客户端、WebSocket 客户端等的库。
*/
