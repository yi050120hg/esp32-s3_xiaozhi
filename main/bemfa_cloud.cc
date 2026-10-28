/**
 * @file bemfa_cloud.cc
 * @brief 巴法云远程控制模块实现
 *
 * @details
 * TCP客户端任务流程（参考巴法云TCP创客云协议）：
 * 1. DNS解析 + TCP连接 bemfa.com:8344（失败5秒后重试）
 * 2. 发送订阅指令：cmd=1&uid=<私钥>&topic=<控制主题>
 * 3. 接收循环：recv设置1秒超时，便于周期执行心跳与状态上报
 *    - 心跳：每60秒发送 "ping\r\n"
 *    - 状态上报：每30秒向数据主题发布植物状态
 *    - 下行指令：cmd=2&uid=..&topic=..&msg=..，解析后触发动作
 * 4. 断线后3秒自动重连
 *
 * 指令触发浇水/补光时，通过Application::Schedule()投递到主循环，
 * 与MCP工具共用同一条线程安全路径；来源标签为"远程浇水"/"远程补光"，
 * 会写入养护日志并复用现有看板状态显示。
 */

#include "bemfa_cloud.h"
#include "application.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <errno.h>

static const char* TAG = "BEMFA";

/**
 * @brief 启动巴法云客户端（幂等）
 */
void BemfaCloud::Start() {
    if (started_) {
        return;  // 已启动，避免重复创建任务
    }
    started_ = true;
    if (xTaskCreate(TaskEntry, "bemfa_cloud", 4096, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "创建巴法云任务失败");
        started_ = false;
    }
}

/**
 * @brief FreeRTOS任务入口
 */
void BemfaCloud::TaskEntry(void* arg) {
    static_cast<BemfaCloud*>(arg)->Run();
}

/**
 * @brief TCP连接主循环（永不返回）
 */
void BemfaCloud::Run() {
    char rx[256];

    while (true) {
        /* DNS解析（WiFi未就绪时失败，稍后重试） */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(BAFA_SERVER_HOST, nullptr, &hints, &res) != 0 || res == nullptr) {
            ESP_LOGW(TAG, "域名解析失败（网络未就绪？），5秒后重试");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        struct sockaddr_in addr;
        memcpy(&addr, res->ai_addr, sizeof(addr));
        addr.sin_port = htons(BAFA_SERVER_PORT);
        freeaddrinfo(res);

        /* 创建socket并连接 */
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            ESP_LOGE(TAG, "创建socket失败");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ESP_LOGW(TAG, "连接巴法云失败，5秒后重试");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 接收超时1秒：让心跳和状态上报能周期性执行 */
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* 订阅全部控制主题（status/water/补光） */
        char cmd[192];
        snprintf(cmd, sizeof(cmd), "cmd=1&uid=%s&topic=%s\r\n",
                 BAFA_UID, BAFA_TOPIC_ALL);
        write(sock, cmd, strlen(cmd));
        sock_ = sock;  // 记录当前连接，供指令回调里即时上报状态
        ESP_LOGI(TAG, "已连接巴法云，订阅主题 %s", BAFA_TOPIC_ALL);

        TickType_t last_hb = xTaskGetTickCount();
        TickType_t last_pub = last_hb;
        bool connected = true;

        while (connected) {
            int len = recv(sock, rx, sizeof(rx) - 1, 0);
            if (len > 0) {
                HandleMessage(rx, len);
            } else if (len == 0) {
                connected = false;  // 服务器关闭连接
            } else if (errno != EAGAIN) {
                connected = false;  // 非超时的其他错误
            }

            TickType_t now = xTaskGetTickCount();
            /* 心跳保活 */
            if ((now - last_hb) >= pdMS_TO_TICKS(BAFA_HEARTBEAT_MS)) {
                write(sock, "ping\r\n", 6);
                last_hb = now;
            }
            /* 周期上报植物状态 */
            if ((now - last_pub) >= pdMS_TO_TICKS(BAFA_STATUS_PERIOD_MS)) {
                PublishStatus(sock);
                last_pub = now;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        sock_ = -1;  // 连接已断开
        close(sock);
        ESP_LOGW(TAG, "巴法云连接断开，3秒后重连");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/**
 * @brief 下行数据按行拆分（巴法云消息以\r\n结尾）
 * @param data 接收缓冲
 * @param len 数据长度
 * @note TCP是流式协议：一次recv可能包含多条消息，逐行拆分
 *       交给HandleLine处理，避免粘连导致误解析
 */
void BemfaCloud::HandleMessage(const char* data, int len) {
    char buf[192];
    int pos = 0;
    while (pos < len) {
        /* 找到当前行的结尾（'\n'） */
        int line_len = 0;
        while (pos + line_len < len && data[pos + line_len] != '\n') {
            line_len++;
        }
        int start = pos;
        pos += line_len + 1;  // 跳过行尾'\n'
        if (line_len >= (int)sizeof(buf)) {
            continue;  // 超长行直接丢弃
        }
        if (line_len > 0 && data[start + line_len - 1] == '\r') {
            line_len--;  // 去掉行尾'\r'
        }
        if (line_len == 0) {
            continue;
        }
        memcpy(buf, data + start, line_len);
        buf[line_len] = '\0';
        HandleLine(buf);
    }
}

/**
 * @brief 处理单条巴法云消息
 * @param msg 单条消息字符串（不含行尾\r\n）
 */
void BemfaCloud::HandleLine(char* msg) {
    ESP_LOGI(TAG, "收到消息: %s", msg);

    /* 心跳应答（cmd=0）直接忽略 */
    if (strstr(msg, "cmd=0") != nullptr) {
        return;
    }

    /* 提取 topic 和 msg 字段 */
    const char* topic_ptr = strstr(msg, "topic=");
    const char* msg_ptr = strstr(msg, "msg=");
    if (topic_ptr == nullptr || msg_ptr == nullptr) {
        return;
    }

    char topic[48] = {0};
    char value[64] = {0};

    /* topic：从"topic="后取到'&'为止 */
    const char* p = topic_ptr + 6;
    int i = 0;
    while (*p != '\0' && *p != '&' && i < (int)sizeof(topic) - 1) {
        topic[i++] = *p++;
    }

    /* msg：从"msg="后取到'\r'、'\n'、'&'或结尾为止 */
    p = msg_ptr + 4;
    i = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n' && *p != '&' &&
           i < (int)sizeof(value) - 1) {
        value[i++] = *p++;
    }

    /* 保险：状态回显消息直接忽略——状态文本含"浇水:/补光:"字样，
       若被关键词匹配会误判成指令造成循环触发 */
    if (strstr(value, "土壤湿度:") != nullptr) {
        return;
    }

    /* 只处理已订阅主题的消息（本模块只订阅指令主题，无需再过滤） */
    (void)topic;

    /* 数字指令优先：1=上报状态 2=浇水 3=补光 */
    if (strcmp(value, "1") == 0) {
        ESP_LOGI(TAG, "远程指令：上报植物状态");
        if (sock_ >= 0) {
            PublishStatus(sock_);
        }
        return;
    }
    if (strcmp(value, "2") == 0) {
        /* 远程指令：浇水（投递到主循环执行，日志记为"远程浇水"） */
        ESP_LOGI(TAG, "远程指令：浇水");
        Application::GetInstance().Schedule([] {
            auto& app = Application::GetInstance();
            app.GetLedCtrl().led_on();
            app.GetPlantMonitor().Water("远程浇水");
        });
        return;
    }
    if (strcmp(value, "3") == 0) {
        /* 远程指令：补光（投递到主循环执行，日志记为"远程补光"） */
        ESP_LOGI(TAG, "远程指令：补光");
        Application::GetInstance().Schedule([] {
            auto& app = Application::GetInstance();
            app.GetLedCtrl().led_on();
            app.GetPlantMonitor().Light("远程补光");
        });
        return;
    }

    /* 关键词指令（兼容保留），ASCII部分做小写宽松匹配 */
    char lower[64];
    int j = 0;
    for (; value[j] != '\0' && j < (int)sizeof(lower) - 1; j++) {
        lower[j] = (value[j] >= 'A' && value[j] <= 'Z') ? value[j] + 32 : value[j];
    }
    lower[j] = '\0';

    if (strstr(value, "浇水") != nullptr || strstr(lower, "water") != nullptr) {
        /* 远程指令：浇水（投递到主循环执行，日志记为"远程浇水"） */
        ESP_LOGI(TAG, "远程指令：浇水");
        Application::GetInstance().Schedule([] {
            auto& app = Application::GetInstance();
            app.GetLedCtrl().led_on();
            app.GetPlantMonitor().Water("远程浇水");
        });
    } else if (strstr(value, "补光") != nullptr || strstr(lower, "light") != nullptr) {
        /* 远程指令：补光（投递到主循环执行，日志记为"远程补光"） */
        ESP_LOGI(TAG, "远程指令：补光");
        Application::GetInstance().Schedule([] {
            auto& app = Application::GetInstance();
            app.GetLedCtrl().led_on();
            app.GetPlantMonitor().Light("远程补光");
        });
    } else {
        ESP_LOGW(TAG, "无法识别的远程指令: %s", value);
    }
}

/**
 * @brief 上报植物状态到数据主题
 * @param sock 已连接的socket描述符
 * @note 全部使用整数格式化，规避nano格式库不支持%f的问题
 */
void BemfaCloud::PublishStatus(int sock) {
    auto& app = Application::GetInstance();
    auto& plant = app.GetPlantMonitor();

    float temperature = 0.0f, humidity = 0.0f;
    app.GetDht11Sensor().GetTemperatureAndHumidity(temperature, humidity);

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "cmd=2&uid=%s&topic=%s&msg=土壤湿度:%d%%,光照:%d%%,温度:%d℃,湿度:%d%%,"
             "浇水:%s,补光:%s\r\n",
             BAFA_UID, BAFA_TOPIC_STATUS,
             plant.GetSoilMoisture(), plant.GetLightLevel(),
             (int)temperature, (int)humidity,
             plant.IsWatering() ? "进行中" : "停止",
             plant.IsLighting() ? "进行中" : "停止");
    write(sock, cmd, strlen(cmd));
}
