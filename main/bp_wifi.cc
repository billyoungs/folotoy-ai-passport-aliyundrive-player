// main/bp_wifi.cc —— WifiManager / SsidManager 单例的极薄封装(纯 STA)。
#include "bp_wifi.h"

#include "ssid_manager.h"
#include "wifi_configuration_ap.h"
#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <string.h>

#include <mutex>

static bp_wifi_evt_cb_t s_cb = nullptr;

static int evt_code(WifiEvent e) {
    switch (e) {
        case WifiEvent::Scanning:     return 0;
        case WifiEvent::Connecting:   return 1;
        case WifiEvent::Connected:    return 2;
        case WifiEvent::Disconnected: return 3;
    }
    return 9;
}

void bp_wifi_init(bp_wifi_evt_cb_t cb) {
    s_cb = cb;
    WifiManagerConfig cfg;
    cfg.station_hostname = "aliyunpan-pocket-player";
    cfg.station_scan_min_interval_seconds = 5;
    cfg.station_scan_max_interval_seconds = 30;
    if (!WifiManager::GetInstance().Initialize(cfg)) {
        ESP_LOGE("bp_wifi", "WifiManager init failed");
        return;
    }
    WifiManager::GetInstance().SetEventCallback(
        [](WifiEvent e, const std::string &d) {
            if (s_cb) s_cb(evt_code(e), d.c_str());
        });
}

int bp_wifi_saved_count(void) {
    return (int)SsidManager::GetInstance().GetSsidList().size();
}

void bp_wifi_start_station(void) { WifiManager::GetInstance().StartStation(); }

// SsidManager 自身无锁,扫描回调每轮都读列表:先停 STA 再改,改完重启并立即重扫。
int bp_wifi_add(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || !pass) return -1;
    bp_wifi_config_stop();   // 配网热点开着时先关掉,避免两边争用 radio
    WifiManager &m = WifiManager::GetInstance();
    m.StopStation();
    esp_wifi_stop();
    SsidManager &s = SsidManager::GetInstance();
    const auto &list = s.GetSsidList();
    for (int i = (int)list.size() - 1; i >= 0; i--)
        if (list[i].ssid == ssid) s.RemoveSsid(i);
    s.AddSsid(ssid, pass);
    m.StartStation();
    ESP_LOGI("bp_wifi", "saved network and restarted STA: %s", ssid);
    return 0;
}

// 测试专用:只写 NVS,不启停 STA。开机初始化阶段调用,随后走正常 StartStation 流程。
int bp_wifi_force_ssid(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || !pass) return -1;
    SsidManager &s = SsidManager::GetInstance();
    const auto &list = s.GetSsidList();
    for (int i = (int)list.size() - 1; i >= 0; i--)
        if (list[i].ssid == ssid) s.RemoveSsid(i);
    s.AddSsid(ssid, pass);
    ESP_LOGI("bp_wifi", "hardcoded test SSID saved: %s", ssid);
    return 0;
}

int bp_wifi_saved_get(int idx, char *out, size_t cap) {
    const auto &list = SsidManager::GetInstance().GetSsidList();
    if (idx < 0 || idx >= (int)list.size() || !out || cap == 0) return -1;
    strlcpy(out, list[idx].ssid.c_str(), cap);
    return 0;
}

int bp_wifi_del(int idx) {
    if (idx < 0 || idx >= bp_wifi_saved_count()) return -1;
    bp_wifi_config_stop();
    WifiManager &m = WifiManager::GetInstance();
    m.StopStation();
    SsidManager::GetInstance().RemoveSsid(idx);
    m.StartStation();
    return 0;
}

bool bp_wifi_is_connected(void) { return WifiManager::GetInstance().IsConnected(); }

void bp_wifi_get_ssid(char *out, size_t cap) {
    std::string s = WifiManager::GetInstance().GetSsid();
    strlcpy(out, s.c_str(), cap);
}

void bp_wifi_get_ip(char *out, size_t cap) {
    std::string s = WifiManager::GetInstance().GetIpAddress();
    strlcpy(out, s.c_str(), cap);
}

int bp_wifi_get_rssi(void) { return WifiManager::GetInstance().GetRssi(); }

// ---- SoftAP 网页配网 ------------------------------------------------------------
static std::mutex s_cfg_mtx;
static WifiConfigurationAp *s_cfg_ap = nullptr;
static esp_netif_t *s_cfg_sta_netif = nullptr;
static char s_cfg_ssid[33];

int bp_wifi_config_start(void) {
    std::lock_guard<std::mutex> lock(s_cfg_mtx);
    if (s_cfg_ap) return 0;
    WifiManager::GetInstance().StopStation();
    esp_wifi_stop();
    // StopStation 会销毁 STA 接口;网页提交后的试连需要它拿 IP 才算成功。
    s_cfg_sta_netif = esp_netif_create_default_wifi_sta();
    s_cfg_ap = new WifiConfigurationAp();
    s_cfg_ap->SetSsidPrefix("AliyunPlayer");
    s_cfg_ap->SetLanguage("zh-CN");
    // 完成页的 /exit 在组件自建的任务里回调,这里只做关闭与回连。
    s_cfg_ap->OnExitRequested([]() { bp_wifi_config_stop(); });
    s_cfg_ap->Start();
    strlcpy(s_cfg_ssid, s_cfg_ap->GetSsid().c_str(), sizeof(s_cfg_ssid));
    ESP_LOGI("bp_wifi", "config AP started: %s", s_cfg_ssid);
    return 0;
}

void bp_wifi_config_stop(void) {
    std::lock_guard<std::mutex> lock(s_cfg_mtx);
    if (!s_cfg_ap) return;
    s_cfg_ap->Stop();
    delete s_cfg_ap;
    s_cfg_ap = nullptr;
    if (s_cfg_sta_netif) {
        esp_netif_destroy_default_wifi(s_cfg_sta_netif);
        s_cfg_sta_netif = nullptr;
    }
    WifiManager::GetInstance().StartStation();
    ESP_LOGI("bp_wifi", "config AP stopped, station restarted");
}

bool bp_wifi_config_active(void) { return s_cfg_ap != nullptr; }

void bp_wifi_config_ssid(char *out, size_t cap) {
    std::lock_guard<std::mutex> lock(s_cfg_mtx);
    strlcpy(out, s_cfg_ap ? s_cfg_ssid : "", cap);
}
