// main/bp_console.c —— USB 串口命令(每行以 \n 结尾,命令词不区分大小写,参数原样):
//   WIFI SET <ssid>|<password>   保存网络并立即连接(无 | 时按第一个空格分隔)
//   WIFI LIST / WIFI DEL <n> / WIFI INFO
//   WIFI AP / WIFI AP STOP       开启 / 关闭 SoftAP 网页配网热点
//   BAIDU AUTH / BAIDU STATUS / BAIDU LOGOUT
//   STATE                        当前页面、网络、授权、播放状态
//   LIST                         打印当前列表页(目录/文件、游标)
//   HEAP                         空闲堆 / 最大连续块 / 历史最低
//   STOP                         停止播放
//   KEY UP|OK|DOWN [CLICK|LONG|DOUBLE]  模拟按键(无手环境验收)
//   UI <n>                       跳到第 n 页(0 播放 1 列表 2 授权 3 设置 4 网络 5 账号 6 关于 7 配网热点 8 亮度 9 播放界面)
// 不打印密码与令牌。
#include "bp_app.h"

#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "bp_console";

static void cmd_wifi(const char *arg) {
    if (!strncasecmp(arg, "SET ", 4)) {
        const char *rest = arg + 4;
        while (*rest == ' ') rest++;
        char ssid[33] = {0}, pass[65] = {0};
        const char *sep = strchr(rest, '|');
        if (!sep) sep = strchr(rest, ' ');
        size_t n = sep ? (size_t)(sep - rest) : strlen(rest);
        if (!n || n >= sizeof(ssid)) {
            printf("WIFI: usage WIFI SET <ssid>|<password>\r\n");
            return;
        }
        memcpy(ssid, rest, n);
        if (sep) strlcpy(pass, sep + 1, sizeof(pass));
        printf("WIFI: %s '%s'\r\n", bp_wifi_add(ssid, pass) == 0 ? "saved" : "failed", ssid);
    } else if (!strncasecmp(arg, "AP STOP", 7)) {
        bp_wifi_config_stop();
        printf("WIFI: config AP stopped\r\n");
    } else if (!strncasecmp(arg, "AP", 2)) {
        int rc = bp_wifi_config_start();
        char ssid[33];
        bp_wifi_config_ssid(ssid, sizeof(ssid));
        printf("WIFI: config AP rc=%d ssid=%s url=http://192.168.4.1\r\n", rc, ssid);
        if (rc == 0 && bsp_lvgl_lock(500)) {
            bp_ui_goto(BP_PAGE_WIFI_AP);
            bsp_lvgl_unlock();
        }
    } else if (!strncasecmp(arg, "LIST", 4)) {
        int n = bp_wifi_saved_count();
        printf("WIFI: %d saved\r\n", n);
        for (int i = 0; i < n; i++) {
            char ssid[33];
            if (bp_wifi_saved_get(i, ssid, sizeof(ssid)) == 0) printf("  [%d] %s\r\n", i, ssid);
        }
    } else if (!strncasecmp(arg, "DEL ", 4)) {
        printf("WIFI: del %s\r\n", bp_wifi_del(atoi(arg + 4)) == 0 ? "ok" : "failed");
    } else {
        char ssid[33] = "", ip[20] = "";
        if (g_bp.wifi_up) {
            bp_wifi_get_ssid(ssid, sizeof(ssid));
            bp_wifi_get_ip(ip, sizeof(ip));
        }
        printf("WIFI: up=%d ssid=%s ip=%s rssi=%d saved=%d\r\n", g_bp.wifi_up, ssid, ip,
               g_bp.wifi_up ? bp_wifi_get_rssi() : 0, bp_wifi_saved_count());
    }
}

static void cmd_aliyun(const char *arg) {
    if (!strncasecmp(arg, "TOKEN ", 6)) {
        // 写入 refresh_token 并触发授权。令牌是敏感数据,只回显首尾各 2 位。
        const char *rt = arg + 6;
        while (*rt == ' ' || *rt == '\t') rt++;
        size_t len = strlen(rt);
        // 去掉尾部空白(复制时可能带上空格/换行)。
        while (len && (rt[len - 1] == '\r' || rt[len - 1] == '\n' ||
                       rt[len - 1] == ' ' || rt[len - 1] == '\t'))
            len--;
        // 浏览器控制台输出字符串时带引号(如 'xxxx'),去掉首尾包裹的引号。
        if (len >= 2 && ((rt[0] == '"' && rt[len - 1] == '"') ||
                         (rt[0] == '\'' && rt[len - 1] == '\''))) {
            rt++;
            len -= 2;
        }
        if (len < 8 || len >= 160) {
            printf("ALI: usage ALI TOKEN <refresh_token>\r\n");
            return;
        }
        char tmp[160];
        memcpy(tmp, rt, len);
        tmp[len] = 0;
        // —— 诊断:逐字符校验并打印长度/首尾各 8 位,用于和浏览器里的原始 token 对比,
        // 一次性判定是"令牌已失效"还是"串口粘贴丢字符/混入非法字符"。——
        int bad = 0;
        for (size_t i = 0; i < len; i++)
            if (!((tmp[i] >= '0' && tmp[i] <= '9') ||
                  (tmp[i] >= 'a' && tmp[i] <= 'f') ||
                  (tmp[i] >= 'A' && tmp[i] <= 'F'))) bad++;
        size_t show = len < 16 ? len : 8;
        printf("ALI: diag len=%u badchar=%d head=%.*s tail=%.*s\r\n",
               (unsigned)len, bad, (int)show, tmp,
               (int)(len >= 16 ? 8 : 0), len >= 16 ? tmp + len - 8 : "");
        int rc = bp_aliyun_set_refresh_token(tmp);
        char mask[32];
        snprintf(mask, sizeof(mask), "%.2s…%.2s", tmp, tmp + len - 2);
        printf("ALI: token %s rc=%d\r\n", mask, rc);
        if (rc == 0 || rc == 1) {
            if (bsp_lvgl_lock(500)) {
                bp_ui_goto(BP_PAGE_AUTH);
                bsp_lvgl_unlock();
            }
        }
    } else if (!strncasecmp(arg, "LOGOUT", 6)) {
        printf("ALI: logout rc=%d\r\n", bp_aliyun_logout());
    } else if (!strncasecmp(arg, "DRIVE ", 6)) {
        // 诊断:手动指定活动盘并触发一次"全部音频"列表,验证指定盘的响应内容。
        int rc = bp_aliyun_set_drive(arg + 6);
        char d[80], r[80];
        bp_aliyun_drive_info(d, sizeof(d), r, sizeof(r));
        printf("ALI: drive rc=%d drive=%s resource=%s\r\n", rc, d, r);
        if (rc == 0) {
            bp_list_req_t req;
            memset(&req, 0, sizeof(req));
            req.source = BP_SRC_ALL_AUDIO;
            strlcpy(req.dir, "/", sizeof(req.dir));
            printf("ALI: scan rc=%d (用 LIST 查看结果)\r\n", bp_aliyun_list_request(&req));
        }
    } else if (!strncasecmp(arg, "QR", 2)) {
        // 诊断:直接触发设备屏幕扫码登录(手机阿里云盘 App)。
        printf("ALI: qr start rc=%d (屏幕将显示二维码)\r\n", bp_aliyun_qr_start());
    } else if (!strncasecmp(arg, "SHOW", 4)) {
        char d[80], r[80];
        bp_aliyun_drive_info(d, sizeof(d), r, sizeof(r));
        printf("ALI: drive=%s resource=%s state=%d\r\n", d, r, bp_aliyun_state());
    } else if (!strncasecmp(arg, "SCAN", 4)) {
        // 诊断:直接按当前盘触发一次"全部音频"列表,免按键导航。
        bp_list_req_t req;
        memset(&req, 0, sizeof(req));
        req.source = BP_SRC_ALL_AUDIO;
        strlcpy(req.dir, "/", sizeof(req.dir));
        printf("ALI: scan rc=%d (用 LIST 查看结果)\r\n", bp_aliyun_list_request(&req));
    } else {
        printf("ALI: state=%d (0 none, 1 waiting, 2 ready)\r\n", bp_aliyun_state());
    }
}

static void cmd_state(void) {
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    printf("STATE: page=%d wifi=%d bd=%d off=%d vol=%u bri=%u skin=%u play=%d pos=%lu/%lu idx=%d/%d "
           "rate=%lu ch=%u kbps=%lu buf=%u%% err=%s name=%s\r\n",
           bp_test_page(), g_bp.wifi_up, bp_aliyun_state(), g_bp.screen_off, g_bp.volume, g_bp.brightness,
           g_bp.skin, pi.state, (unsigned long)pi.pos_ms, (unsigned long)pi.total_ms, pi.index + 1,
           pi.count, (unsigned long)pi.rate, pi.channels, (unsigned long)pi.kbps,
           pi.buffer_pct, pi.error ? pi.error : "-", pi.name);
}

static void cmd_list(void) {
    static bp_list_t list;   // 约 1.7 KiB,不放在任务栈上
    bp_aliyun_list_snapshot(&list);
    printf("LIST: src=%d dir=%s page=%d marker=%s next=%s more=%d status=%d count=%d\r\n",
           list.req.source, list.req.dir, list.req.page, list.req.marker,
           list.next_marker, list.has_more, list.status, list.count);
    for (int i = 0; i < list.count; i++)
        printf("  [%d] %s %s\r\n", i, list.files[i].is_dir ? "DIR " : "FILE", list.files[i].name);
}

static void console_task(void *arg) {
    (void)arg;
    char line[160], up[160];
    size_t n = 0;
    printf("bp-console: ready (WIFI SET s|p / WIFI LIST / ALI TOKEN <rt> / STATE / KEY / UI)\r\n");
    for (;;) {
        // ESP-IDF 的 UART VFS 是非规范模式:每次 read 只返回当前已到达的字节,
        // 无数据时返回 0(EOF)。因此不能用 fgets 等整行,必须逐字符累积到换行再解析。
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (n == 0) continue;   // 空行直接丢弃
            line[n] = 0;
            n = 0;
            strlcpy(up, line, sizeof(up));
            for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
            if (!strncmp(up, "WIFI", 4)) {
                const char *a = line + 4;
                while (*a == ' ') a++;
                cmd_wifi(a);
            } else if (!strncmp(up, "ALI", 3)) {
                // 关键:参数必须取原始大小写的 line,不能取 up!
                // up 已被整行 toupper,refresh_token 中的小写字母 a-f 若被转成 A-F,
                // 阿里云盘服务器按大小写敏感校验,必然返回 InvalidParameter.RefreshToken。
                const char *a = line + 3;
                while (*a == ' ') a++;
                cmd_aliyun(a);
            } else if (!strcmp(up, "STATE")) {
                cmd_state();
            } else if (!strcmp(up, "LIST")) {
                cmd_list();
            } else if (!strcmp(up, "HEAP")) {
                printf("HEAP: free=%u largest=%u min=%u\r\n", (unsigned)esp_get_free_heap_size(),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                       (unsigned)esp_get_minimum_free_heap_size());
            } else if (!strcmp(up, "STOP")) {
                bp_player_stop();
                printf("STOP: ok\r\n");
            } else if (!strncmp(up, "KEY ", 4)) {
                char button[12] = "", event[12] = "CLICK";
                int n = sscanf(up + 4, "%11s %11s", button, event);
                int b = n > 0 && !strcmp(button, "UP") ? 0 : n > 0 && !strcmp(button, "OK") ? 1 :
                        n > 0 && !strcmp(button, "DOWN") ? 2 : -1;
                int kind = !strcmp(event, "CLICK") ? 0 : !strcmp(event, "LONG") ? 1 :
                           !strcmp(event, "DOUBLE") ? 2 : -1;
                if (b < 0 || kind < 0)
                    printf("KEY: usage KEY UP|OK|DOWN [CLICK|LONG|DOUBLE]\r\n");
                else
                    printf("KEY: rc=%d\r\n", bp_test_key(b, kind));
            } else if (!strncmp(up, "UI", 2)) {
                int pg = atoi(up + 2);
                if (pg < 0 || pg >= BP_PAGE_COUNT) {
                    printf("UI: 0..%d\r\n", BP_PAGE_COUNT - 1);
                } else if (bsp_lvgl_lock(500)) {
                    bp_ui_goto((bp_page_t)pg);
                    bsp_lvgl_unlock();
                    printf("UI: page %d\r\n", pg);
                }
            } else if (line[0]) {
                printf("bp-console: unknown '%s'\r\n", line);
            }
        } else if (c == 0x08 || c == 0x7f) {
            // 退格:删除已累积的最后一个字符
            if (n > 0) n--;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

void bp_console_start(void) {
    if (xTaskCreate(console_task, "bp_console", 5120, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "console task create failed");
}
