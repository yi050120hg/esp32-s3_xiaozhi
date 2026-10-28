/**
 * @file plant_monitor.h
 * @brief 植物养护监测模块头文件（虚拟数据）
 *
 * @details
 * 本文件定义了PlantMonitor类，用于模拟智能绿植养护系统中的土壤湿度、
 * 光照强度、浇水状态以及补光状态。所有数据均为虚拟生成，用于demo演示。
 *
 * 功能说明：
 * - 土壤湿度：随浇水升高，随时间缓慢下降
 * - 光照强度：围绕环境基准小幅波动；补光时快速升高，补光结束后逐步回落
 * - 浇水状态：调用Water()后进入浇水过程，Tick()每秒推进
 * - 补光状态：调用Light()后进入补光过程，Tick()每秒推进
 * - 演示设置：SetSoilMoisture()/SetLightLevel()可手动设定数值，
 *   方便在低湿度/低光照场景下向他人演示浇水和补光功能
 * - 养护日志：浇水/补光/演示设置等事件自动记录到环形缓冲（最近20条），
 *   通过GetHistory()以JSON返回，供MCP工具做语音查询
 *
 * 使用示例：
 * @code
 * PlantMonitor plant;
 * plant.Initialize();
 * plant.Water();                 // 开始浇水
 * plant.Light();                  // 开始补光
 * plant.SetLightLevel(10);        // 演示：手动把光照调到10%
 * int events = plant.Tick();      // 每秒钟调用一次推进虚拟数据
 * if (events & PLANT_EVENT_LIGHTING_DONE) { ... }
 * @endcode
 */

#ifndef PLANT_MONITOR_H
#define PLANT_MONITOR_H

#include <string>
#include <ctime>

/// Tick()事件：无事件发生
#define PLANT_EVENT_NONE           0x00
/// Tick()事件：一次浇水刚刚结束
#define PLANT_EVENT_WATERING_DONE  0x01
/// Tick()事件：一次补光刚刚结束
#define PLANT_EVENT_LIGHTING_DONE  0x02
/// Tick()事件：自动养护模式触发了浇水
#define PLANT_EVENT_AUTO_WATER    0x04
/// Tick()事件：自动养护模式触发了补光
#define PLANT_EVENT_AUTO_LIGHT    0x08

/**
 * @brief 植物养护监测类
 *
 * @note 该类提供虚拟数据，不连接实际硬件
 * @note 该类为单例成员，建议在Application类中作为成员变量使用
 */
class PlantMonitor {
public:
    /**
     * @brief 构造函数
     */
    PlantMonitor();

    /**
     * @brief 析构函数
     */
    ~PlantMonitor();

    /**
     * @brief 初始化模块
     *
     * 生成初始虚拟土壤湿度和光照强度。
     *
     * @return bool 初始化成功返回true
     */
    bool Initialize();

    /**
     * @brief 获取当前土壤湿度
     * @return int 土壤湿度百分比（0-100）
     */
    int GetSoilMoisture() const { return soil_moisture_; }

    /**
     * @brief 获取当前光照强度
     * @return int 光照强度百分比（0-100）
     */
    int GetLightLevel() const { return light_level_; }

    /**
     * @brief 是否正在浇水
     * @return bool true表示正在浇水
     */
    bool IsWatering() const { return watering_; }

    /**
     * @brief 是否正在补光
     * @return bool true表示正在补光
     */
    bool IsLighting() const { return lighting_; }

    /**
     * @brief 开始浇水
     *
     * 进入浇水状态，土壤湿度会在后续Tick()中逐步升高。
     *
     * @param label 日志来源标签：默认"开始浇水"（语音指令）；
     *              自动养护传"自动浇水"，远程控制传"远程浇水"
     */
    void Water(const char* label = nullptr);

    /**
     * @brief 开始补光（模拟补光灯）
     *
     * 进入补光状态，光照强度会在后续Tick()中逐步升高；
     * 结束后光照强度逐步回落到环境基准。
     *
     * @param label 日志来源标签：默认"开始补光"（语音指令）；
     *              自动养护传"自动补光"，远程控制传"远程补光"
     */
    void Light(const char* label = nullptr);

    /**
     * @brief 手动设置土壤湿度（演示/测试用）
     * @param value 土壤湿度百分比（0-100，超出范围会被截断）
     */
    void SetSoilMoisture(int value);

    /**
     * @brief 手动设置光照强度（演示/测试用）
     * @param value 光照强度百分比（0-100，超出范围会被截断）
     * @note 环境光照基准会跟随该设定值，光照将在设定值附近波动，
     *       便于保持低光照场景进行补光演示。
     */
    void SetLightLevel(int value);

    /**
     * @brief 获取养护日志（语音查询用）
     *
     * 返回最近20条养护事件的JSON字符串，按时间倒序（最新在前），
     * 每条包含时间标签和事件描述。通过MCP工具暴露给AI，
     * 可支持"今天浇了几次水/做过哪些养护操作"等语音查询。
     *
     * @return std::string JSON格式日志，如：
     *         {"count":2,"events":[{"time":"14:32","event":"浇水完成(土壤湿度28%)"}]}
     */
    std::string GetHistory() const;

    /**
     * @brief 开启/关闭自动养护模式
     *
     * 开启后，Tick()中检测到土壤湿度低于20%自动浇水、
     * 光照强度低于15%自动补光（两次自动动作间隔至少30秒）。
     * 每次切换会记录到养护日志。
     *
     * @param enabled true开启，false关闭
     */
    void SetAutoCare(bool enabled);

    /**
     * @brief 自动养护模式是否开启
     * @return bool true表示已开启
     */
    bool IsAutoCare() const { return auto_care_; }

    /**
     * @brief 获取自动养护提示文本（供看板状态行显示）
     *
     * 自动浇水/补光触发后10秒内返回"已自动浇水"/"已自动补光"
     * 等提示文本，超时后返回空字符串。
     *
     * @return const char* 提示文本，无提示时为空字符串
     */
    const char* GetAutoStatusText() const;

    /**
     * @brief 推进虚拟数据（每秒调用一次）
     *
     * - 浇水过程中升高土壤湿度，结束时置位PLANT_EVENT_WATERING_DONE
     * - 补光过程中升高光照强度，结束时置位PLANT_EVENT_LIGHTING_DONE
     * - 非浇水状态土壤湿度缓慢下降
     * - 非补光状态光照强度围绕环境基准波动，补光结束后逐步回落
     * - 自动养护模式开启时：土壤湿度低于阈值自动浇水（置位PLANT_EVENT_AUTO_WATER）、
     *   光照强度低于阈值自动补光（置位PLANT_EVENT_AUTO_LIGHT），
     *   两次自动动作之间至少间隔PLANT_AUTO_COOLDOWN_SECONDS秒
     *
     * @return int 事件位掩码（NONE / WATERING_DONE / LIGHTING_DONE /
     *                       AUTO_WATER / AUTO_LIGHT）
     */
    int Tick();

private:
    /**
     * @brief 记录一条养护日志（环形缓冲，最多LOG_CAPACITY条）
     * @param event 事件描述（会被复制，最长47字节）
     */
    void RecordEvent(const char* event);

    /// 养护日志条目（定长结构，避免堆碎片）
    struct PlantLogEntry {
        time_t ts;                ///< 事件时刻（服务器时间未同步时为0）
        unsigned int uptime_min;  ///< 开机分钟数（时间未同步时的时间标签）
        char event[48];           ///< 事件描述
    };

    static const int LOG_CAPACITY = 20;  ///< 日志环形缓冲容量
    PlantLogEntry logs_[LOG_CAPACITY];   ///< 日志环形缓冲
    int log_count_;   ///< 已记录条数（0 - LOG_CAPACITY）
    int log_index_;   ///< 下一条写入位置（环形）

    bool auto_care_;          ///< 自动养护模式开关
    int auto_cooldown_;       ///< 自动动作冷却倒计时（秒）
    char auto_status_[32];    ///< 自动养护提示文本（触发后显示10秒）
    int auto_status_left_;    ///< 提示文本剩余显示秒数

    int soil_moisture_;   ///< 土壤湿度（0-100）
    int light_level_;     ///< 光照强度（0-100）
    bool watering_;       ///< 浇水状态标志
    int watering_left_;   ///< 剩余浇水秒数
    int dry_counter_;     ///< 干燥计数，用于控制下降速度
    bool lighting_;       ///< 补光状态标志
    int light_left_;      ///< 剩余补光秒数
    int light_floor_;     ///< 环境光照基准（波动下限）
    bool light_decay_;    ///< 补光结束后的回落标志
};

#endif // PLANT_MONITOR_H
