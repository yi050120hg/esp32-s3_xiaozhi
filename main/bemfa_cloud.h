/**
 * @file bemfa_cloud.h
 * @brief 巴法云远程控制模块（TCP客户端）
 *
 * @details
 * 通过巴法云TCP创客云（bemfa.com:8344）实现植物养护的远程控制：
 * - 订阅控制主题，接收"浇水"/"补光"指令并触发对应动作
 * - 周期性向数据主题上报植物状态（土壤湿度/光照/温湿度等）
 * - 维护TCP长连接：60秒心跳、断线自动重连
 *
 * 指令触发动作通过Application::Schedule()投递到主循环执行，
 * 与MCP工具走同一条线程安全路径；动作会自动写入养护日志、
 * 并复用现有看板的"正在浇水/正在补光"状态显示。
 *
 * @section 使用说明
 * 数字指令（发到任一主题卡片均可，按消息内容识别）：
 * - "1"  → 立即上报植物状态到status主题
 * - "2"  → 触发远程浇水
 * - "3"  → 触发远程补光
 * 关键词指令（兼容保留）：
 * - "浇水" 或 "water"  → 触发远程浇水
 * - "补光" 或 "light"  → 触发远程补光
 *
 * @note 设备发布到status的状态消息会被巴法云回显给自己，回显
 *       内容含"土壤湿度:"标记，HandleLine据此识别并丢弃，防止
 *       状态文本中的"浇水/补光"字样被关键词匹配误判成指令。
 */

#ifndef BEMFA_CLOUD_H
#define BEMFA_CLOUD_H

/* 巴法云服务器配置 */
#define BAFA_SERVER_HOST      "bemfa.com"  /* 服务器域名 */
#define BAFA_SERVER_PORT      8344         /* TCP端口 */

/* 巴法云用户配置 */
#define BAFA_UID              "d5b46c5865ba445ba3c81feb509d84c4"  /* 用户私钥 */

/* 主题配置（设备编码） */
#define BAFA_TOPIC_ALL        "status,water,hLWUuPJ4m002"  /* 订阅的全部主题（逗号分隔）。
                                                             status也在订阅列表：设备发布的状态
                                                             会被巴法云回显回来，由HandleLine里的
                                                             "土壤湿度:"标记检查拦截，不会误触发 */
#define BAFA_TOPIC_STATUS     "status"   /* 数据主题：上报植物状态 */

/* 定时配置 */
#define BAFA_HEARTBEAT_MS     60000  /* 心跳间隔（毫秒） */
#define BAFA_STATUS_PERIOD_MS 30000  /* 状态上报间隔（毫秒） */

/**
 * @brief 巴法云远程控制类
 */
class BemfaCloud {
public:
    BemfaCloud() = default;
    ~BemfaCloud() = default;

    /**
     * @brief 启动巴法云客户端（幂等，可重复调用）
     *
     * 创建独立TCP任务，任务内部自己维护连接、断线重连。
     *
     * @warning 必须在board.StartNetwork()之后调用：
     *          TCP/IP协议栈（esp_netif）就绪前调用getaddrinfo会触发
     *          lwIP断言"tcpip_send_msg_wait_sem (Invalid mbox)"导致设备重启循环。
     *          WiFi未连接时DNS解析失败会自动重试，属正常路径。
     */
    void Start();

private:
    static void TaskEntry(void* arg);   ///< FreeRTOS任务入口
    void Run();                        ///< TCP连接主循环（连接/订阅/收发/重连）
    void HandleMessage(const char* data, int len);  ///< 下行数据按行拆分
    void HandleLine(char* msg);        ///< 处理单条巴法云消息
    void PublishStatus(int sock);       ///< 上报植物状态到数据主题

    bool started_ = false;  ///< 任务是否已启动
    int sock_ = -1;         ///< 当前TCP连接（供指令回调里即时上报状态用）
};

#endif  // BEMFA_CLOUD_H
