// main/bp_wifi.h —— esp-wifi-connect 的 C 桥接(纯 STA)。
//
// 凭证经 SoftAP 网页配网或 USB 串口 `WIFI SET <ssid>|<pass>` 写入 SsidManager(NVS 持久化,最多 10 条);
// 开机自动扫描,已存网络谁在就连谁。
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 事件码: 0 scanning / 1 connecting(ssid) / 2 connected(ssid) / 3 disconnected
typedef void (*bp_wifi_evt_cb_t)(int evt, const char *data);

void bp_wifi_init(bp_wifi_evt_cb_t cb);
int  bp_wifi_saved_count(void);
void bp_wifi_start_station(void);
int  bp_wifi_add(const char *ssid, const char *pass);   // 0 成功,同 SSID 覆盖
// 测试专用:直接写入 SsidManager(NVS 持久化),不启停 STA。
// 用于测试阶段把固定 WiFi 硬编码进固件,避免每次配网。正式发布前应移除调用。
int  bp_wifi_force_ssid(const char *ssid, const char *pass);
int  bp_wifi_saved_get(int idx, char *out, size_t cap); // 0 成功
int  bp_wifi_del(int idx);                              // 0 成功
bool bp_wifi_is_connected(void);
void bp_wifi_get_ssid(char *out, size_t cap);
void bp_wifi_get_ip(char *out, size_t cap);
int  bp_wifi_get_rssi(void);

// ---- SoftAP 网页配网 ------------------------------------------------------------
// 开启开放热点 AliyunPlayer-XXXX + 强制门户(http://192.168.4.1)。网页提交后设备先试连,
// 成功才保存;完成页自动请求 /exit,随后关闭热点并按已存网络重新连接。
int  bp_wifi_config_start(void);              // 0 成功(已在配网也返回 0)
void bp_wifi_config_stop(void);               // 关闭热点并恢复 STA 回连
bool bp_wifi_config_active(void);
void bp_wifi_config_ssid(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
