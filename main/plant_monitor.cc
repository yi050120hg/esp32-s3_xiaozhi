/**
 * @file plant_monitor.cc
 * @brief 植物养护监测模块实现文件（虚拟数据）
 *
 * @details
 * 实现PlantMonitor类，提供虚拟的土壤湿度、光照强度、浇水状态和补光状态数据。
 * 所有数据均由随机数生成或随时间演化，用于demo演示。
 */

#include "plant_monitor.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

static const char* TAG = "PLANT_MONITOR";

/// 单次浇水持续的秒数
#define PLANT_WATERING_SECONDS 5

/// 单次补光持续的秒数
#define PLANT_LIGHTING_SECONDS 5

/// 自动养护：土壤湿度触发阈值（%），低于此值自动浇水
#define PLANT_AUTO_SOIL_THRESHOLD 20
/// 自动养护：光照强度触发阈值（%），低于此值自动补光
#define PLANT_AUTO_LIGHT_THRESHOLD 15
/// 自动养护：两次自动动作之间的最小间隔（秒）
#define PLANT_AUTO_COOLDOWN_SECONDS 30
/// 自动养护：看板提示文本显示时长（秒）
#define PLANT_AUTO_STATUS_SECONDS 10
/// 自动养护：补光后维持的环境光照基准（%），避免补光-回落反复触发
#define PLANT_AUTO_LIGHT_FLOOR 40

PlantMonitor::PlantMonitor()
    : log_count_(0), log_index_(0),
      auto_care_(false), auto_cooldown_(0), auto_status_left_(0),
      soil_moisture_(40), light_level_(60), watering_(false),
      watering_left_(0), dry_counter_(0), lighting_(false),
      light_left_(0), light_floor_(50), light_decay_(false) {
    memset(logs_, 0, sizeof(logs_));
    memset(auto_status_, 0, sizeof(auto_status_));
}

PlantMonitor::~PlantMonitor() {
}

bool PlantMonitor::Initialize() {
    // 初始土壤湿度：30% - 59%
    soil_moisture_ = 30 + (rand() % 30);
    // 初始光照强度：50% - 89%
    light_level_ = 50 + (rand() % 40);
    watering_ = false;
    watering_left_ = 0;
    dry_counter_ = 0;
    lighting_ = false;
    light_left_ = 0;
    light_floor_ = 50;
    light_decay_ = false;
    // 清空养护日志（重启后日志重新开始记录）
    log_count_ = 0;
    log_index_ = 0;
    // 自动养护模式默认关闭
    auto_care_ = false;
    auto_cooldown_ = 0;
    auto_status_[0] = '\0';
    auto_status_left_ = 0;

    ESP_LOGI(TAG, "植物监测模块初始化成功: 土壤湿度=%d%%, 光照=%d%%",
             soil_moisture_, light_level_);
    return true;
}

void PlantMonitor::Water(const char* label) {
    const char* action = label ? label : "开始浇水";
    char event[48];
    snprintf(event, sizeof(event), "%s(土壤湿度%d%%)", action, soil_moisture_);
    RecordEvent(event);
    watering_ = true;
    watering_left_ = PLANT_WATERING_SECONDS;
    ESP_LOGI(TAG, "%s，持续 %d 秒", action, PLANT_WATERING_SECONDS);
}

void PlantMonitor::Light(const char* label) {
    const char* action = label ? label : "开始补光";
    char event[48];
    snprintf(event, sizeof(event), "%s(光照%d%%)", action, light_level_);
    RecordEvent(event);
    lighting_ = true;
    light_left_ = PLANT_LIGHTING_SECONDS;
    ESP_LOGI(TAG, "%s，持续 %d 秒", action, PLANT_LIGHTING_SECONDS);
}

void PlantMonitor::SetSoilMoisture(int value) {
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    soil_moisture_ = value;
    char event[48];
    snprintf(event, sizeof(event), "演示设置:土壤湿度=%d%%", soil_moisture_);
    RecordEvent(event);
    ESP_LOGI(TAG, "演示设置：土壤湿度=%d%%", soil_moisture_);
}

void PlantMonitor::SetLightLevel(int value) {
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    light_level_ = value;
    light_floor_ = value;   // 环境光照基准跟随设定值，光照稳定在设定值附近
    light_decay_ = false;
    char event[48];
    snprintf(event, sizeof(event), "演示设置:光照强度=%d%%", light_level_);
    RecordEvent(event);
    ESP_LOGI(TAG, "演示设置：光照强度=%d%%", light_level_);
}

int PlantMonitor::Tick() {
    int events = PLANT_EVENT_NONE;

    /* 自动养护模式：冷却倒计时与看板提示倒计时 */
    if (auto_cooldown_ > 0) {
        auto_cooldown_--;
    }
    if (auto_status_left_ > 0) {
        if (--auto_status_left_ == 0) {
            auto_status_[0] = '\0';
        }
    }

    /* 自动养护模式：土壤湿度/光照过低时自动浇水、补光 */
    if (auto_care_ && auto_cooldown_ == 0) {
        bool auto_water = (!watering_ && soil_moisture_ < PLANT_AUTO_SOIL_THRESHOLD);
        // 光照需在回落结束后才判定，避免补光-回落过程中重复触发
        bool auto_light = (!lighting_ && !light_decay_ &&
                           light_level_ < PLANT_AUTO_LIGHT_THRESHOLD);
        if (auto_water || auto_light) {
            auto_cooldown_ = PLANT_AUTO_COOLDOWN_SECONDS;
            auto_status_left_ = PLANT_AUTO_STATUS_SECONDS;

            if (auto_water) {
                Water("自动浇水");  // 日志记录为"自动浇水"
                events |= PLANT_EVENT_AUTO_WATER;
            }
            if (auto_light) {
                // 补光后维持健康环境基准，避免光照反复回落到低值再触发
                light_floor_ = PLANT_AUTO_LIGHT_FLOOR;
                Light("自动补光");  // 日志记录为"自动补光"
                events |= PLANT_EVENT_AUTO_LIGHT;
            }
            // 看板提示文本：两个动作同时触发时拼接显示
            if (auto_water && auto_light) {
                snprintf(auto_status_, sizeof(auto_status_), "已自动浇水+补光");
            } else if (auto_water) {
                snprintf(auto_status_, sizeof(auto_status_), "已自动浇水");
            } else {
                snprintf(auto_status_, sizeof(auto_status_), "已自动补光");
            }
        }
    }

    /* 光照强度演化 */
    if (lighting_) {
        // 补光过程中光照强度快速升高
        light_level_ += 10;
        if (light_level_ > 95) {
            light_level_ = 95;
        }

        if (--light_left_ <= 0) {
            lighting_ = false;
            light_decay_ = true;  // 补光结束后逐步回落到环境基准
            events |= PLANT_EVENT_LIGHTING_DONE;
            char event[48];
            snprintf(event, sizeof(event), "补光完成(光照%d%%)", light_level_);
            RecordEvent(event);
            ESP_LOGI(TAG, "补光完成，光照强度=%d%%", light_level_);
        }
    } else if (light_decay_ && light_level_ > light_floor_) {
        // 补光刚结束：快速回落（差值的1/3，至少3），模拟补光灯关闭后环境光恢复
        int drop = (light_level_ - light_floor_) / 3;
        if (drop < 3) drop = 3;
        light_level_ -= drop;
        if (light_level_ <= light_floor_) {
            light_level_ = light_floor_;
            light_decay_ = false;
            ESP_LOGI(TAG, "光照已回落到环境基准=%d%%", light_floor_);
        }
    } else {
        // 正常状态：围绕环境基准小幅随机波动
        light_level_ += (rand() % 3) - 1;
        if (light_level_ < light_floor_) light_level_ = light_floor_;
        if (light_level_ > 95) light_level_ = 95;
    }

    /* 土壤湿度演化 */
    if (watering_) {
        // 浇水过程中土壤湿度快速升高
        soil_moisture_ += 10;
        if (soil_moisture_ > 95) {
            soil_moisture_ = 95;
        }

        if (--watering_left_ <= 0) {
            watering_ = false;
            events |= PLANT_EVENT_WATERING_DONE;
            char event[48];
            snprintf(event, sizeof(event), "浇水完成(土壤湿度%d%%)", soil_moisture_);
            RecordEvent(event);
            ESP_LOGI(TAG, "浇水完成，土壤湿度=%d%%", soil_moisture_);
        }
    } else {
        // 非浇水状态：土壤湿度随时间缓慢下降
        if (++dry_counter_ >= 5) {
            dry_counter_ = 0;
            if (soil_moisture_ > 15) {
                soil_moisture_ -= 1;
            }
        }
    }

    return events;
}

void PlantMonitor::RecordEvent(const char* event) {
    PlantLogEntry& entry = logs_[log_index_];
    entry.ts = time(NULL);
    entry.uptime_min = (unsigned int)(esp_timer_get_time() / 1000000ULL / 60ULL);
    snprintf(entry.event, sizeof(entry.event), "%s", event);
    log_index_ = (log_index_ + 1) % LOG_CAPACITY;
    if (log_count_ < LOG_CAPACITY) {
        log_count_++;
    }
    ESP_LOGI(TAG, "养护日志: %s", event);
}

std::string PlantMonitor::GetHistory() const {
    std::string json = "{\"count\": " + std::to_string(log_count_) + ", \"events\": [";
    // 从最新到最旧输出：最新一条在写指针的前一个位置
    for (int i = 0; i < log_count_; i++) {
        int idx = (log_index_ - 1 - i + LOG_CAPACITY) % LOG_CAPACITY;
        const PlantLogEntry& entry = logs_[idx];
        char time_label[32];
        if (entry.ts > 1700000000) {
            // 服务器时间已同步：显示当天时刻
            struct tm tm_info;
            localtime_r(&entry.ts, &tm_info);
            snprintf(time_label, sizeof(time_label), "%02d:%02d",
                     tm_info.tm_hour, tm_info.tm_min);
        } else {
            // 时间未同步（开机早期）：用开机分钟数代替
            snprintf(time_label, sizeof(time_label), "开机%d分钟", (int)entry.uptime_min);
        }
        if (i > 0) {
            json += ", ";
        }
        json += "{\"time\": \"";
        json += time_label;
        json += "\", \"event\": \"";
        json += entry.event;
        json += "\"}";
    }
    json += "]}";
    return json;
}

void PlantMonitor::SetAutoCare(bool enabled) {
    if (auto_care_ == enabled) {
        return;  // 状态未变化，无需处理
    }
    auto_care_ = enabled;
    RecordEvent(enabled ? "自动养护模式:已开启" : "自动养护模式:已关闭");
    if (enabled) {
        auto_cooldown_ = 0;        // 开启后立即允许触发检查
        auto_status_[0] = '\0';    // 清除旧提示
        auto_status_left_ = 0;
    }
    ESP_LOGI(TAG, "自动养护模式：%s", enabled ? "开启" : "关闭");
}

const char* PlantMonitor::GetAutoStatusText() const {
    return auto_status_;
}
