// main/main.c —— 阿里云盘随身听入口:初始化、按键分发、页面状态机、空闲熄屏、心跳。
//
// 按键(三键:上 / OK / 下)。开机进入播放界面,没有首页:
//   播放页    OK=播放/暂停(还没有曲目时打开“全部音频”)  长按OK=进入设置
//             上/下=音量  长按上/下=上一首/下一首
//   设置菜单  上/下=选择  OK=进入  长按OK=回到播放页
//             (全部音频 / 浏览网盘 / 播放界面 / 屏幕亮度 / 无线网络 / 网盘账号 / 关于与按键)
//   列表      上/下=选择(长按 ±5)  OK=打开文件夹/播放/翻页  长按OK=上一级/设置
//   播放界面  上/下=经典或磁带  OK=应用(保存)  长按OK=返回设置
//   屏幕亮度  上/下=调节 5 档(立即生效并保存,默认 3)  长按OK=返回设置
//   无线网络  OK=开启配网热点  长按OK=返回设置
//   配网热点  长按OK=关闭热点并返回(网页配网完成后自动关闭)
//
// 设置向导:首次开机进入配网热点(第 1 步);联网后若未绑定网盘,自动进入扫码绑定
// (第 2 步);绑定成功直接打开“全部音频”。只在播放/网络/配网/绑定页时自动跳转,
// 用户在绑定页长按 OK 退出后,本次开机不再自动弹出。
//   熄屏时任意键唤醒(该次按键不触发操作)
#include "bp_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

#define IDLE_TIMEOUT_MS 30000

bp_state_t g_bp;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static uint32_t s_last_activity_ms;
static int s_wake_button = -1;
static bool s_logout_armed;
static bool s_auth_declined;   // 用户主动退出过绑定页,本次开机不再自动弹出

// 内部事件:后台任务通过按键队列投递,统一由输入任务切换页面。
#define EVT_WIFI_UP ((bsp_btn_t)100)
#define EVT_AUTHORIZED ((bsp_btn_t)101)

static void post_event(bsp_btn_t evt) {
    const input_event_t in = {.btn = evt, .event = BSP_BTN_CLICK};
    if (s_input_ready) (void)xQueueSend(s_input_queue, &in, 0);
}

void bp_app_on_authorized(void) { post_event(EVT_AUTHORIZED); }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void apply_backlight(void) {
    bsp_display_backlight(g_bp.screen_off ? 0 : bp_brightness_percent(g_bp.brightness));
}

static void screen_off(bool off) {
    if (g_bp.screen_off == off) return;
    g_bp.screen_off = off;
    apply_backlight();
}

#define DEFAULT_BRIGHTNESS 3

// 界面设置(亮度档位、播放界面皮肤)保存在 NVS 的 bp_ui 命名空间。
static void load_ui_prefs(void) {
    uint8_t level = DEFAULT_BRIGHTNESS, skin = BP_SKIN_REEL;
    nvs_handle_t h;
    if (nvs_open("bp_ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bri", &level);
        nvs_get_u8(h, "skin", &skin);
        nvs_close(h);
    }
    g_bp.brightness = (uint8_t)bp_brightness_step(level, 0);
    g_bp.skin = skin < BP_SKIN_COUNT ? skin : BP_SKIN_REEL;
}

static void save_ui_pref(const char *key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open("bp_ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void set_brightness(int level) {
    level = bp_brightness_step(level, 0);
    if (level == g_bp.brightness) return;
    g_bp.brightness = (uint8_t)level;
    apply_backlight();
    save_ui_pref("bri", (uint8_t)level);
}

static void set_skin(bp_skin_t skin) {
    if (skin != g_bp.skin) {
        g_bp.skin = (uint8_t)skin;
        save_ui_pref("skin", (uint8_t)skin);
    }
    if (bsp_lvgl_lock(500)) {
        bp_ui_apply_skin();
        bsp_lvgl_unlock();
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "已切换为%s界面", BP_SKIN_NAMES[skin]);
    bp_ui_toast(msg);
}

static void on_wifi_event(int evt, const char *data) {
    if (evt == 2) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s", data);
        g_bp.wifi_up = true;
        bp_aliyun_on_wifi(true);
        post_event(EVT_WIFI_UP);
    } else if (evt == 3) {
        g_bp.wifi_up = false;
        bp_aliyun_on_wifi(false);
    }
}

// ---- 页面动作(输入任务内调用,LVGL 操作都加锁) --------------------------------
static void go(bp_page_t page) {
    if (bsp_lvgl_lock(500)) {
        bp_ui_goto(page);
        bsp_lvgl_unlock();
    }
}

static void ui_move(int delta) {
    if (bsp_lvgl_lock(500)) {
        bp_ui_move(delta);
        bsp_lvgl_unlock();
    }
}

static void start_web_config(void) {
    bp_player_stop();   // 热点 + HTTP 服务需要内存,且 radio 切到 AP+STA
    if (bp_wifi_config_start() == 0) go(BP_PAGE_WIFI_AP);
    else bp_ui_toast("无法开启热点");
}

static bool require_auth(void) {
    if (bp_aliyun_state() == BP_BD_READY) return true;
    if (!g_bp.wifi_up) {
        bp_ui_toast("请先连接 Wi-Fi");
        go(BP_PAGE_WIFI);
        return false;
    }
    bp_aliyun_auth_start();
    go(BP_PAGE_AUTH);
    return false;
}

// 每页在网盘接口里的分页游标(过滤后页码不能直接换算),用于“上一页”。
static char s_page_marker[BP_PAGE_HISTORY][BP_MARKER_MAX];

// 目录层级栈:浏览网盘时记录每层目录的 file_id,供“返回上级”使用。0 为根目录。
static char s_dir_ids[BP_FOLDER_MAX][BP_FILE_ID_MAX];
static int s_dir_depth;

static void open_list_at(bp_source_t source, const char *dir, const char *dir_file_id,
                         int page, const char *marker) {
    // 浏览网盘时先自动暂停播放(挂起并释放播放占用的内存),在播放页按 OK 继续。
    if (bp_player_suspend()) bp_ui_toast("已暂停播放");
    bp_list_req_t req = {.source = source, .page = page};
    strlcpy(req.marker, marker ? marker : "", sizeof(req.marker));
    strlcpy(req.dir, dir ? dir : "/", sizeof(req.dir));
    strlcpy(req.parent_file_id, dir_file_id ? dir_file_id : "root", sizeof(req.parent_file_id));
    if (page >= 0 && page < BP_PAGE_HISTORY)
        strlcpy(s_page_marker[page], req.marker, BP_MARKER_MAX);
    int rc = bp_aliyun_list_request(&req);
    if (rc == -3) {
        bp_ui_toast("请稍候");
        return;
    }
    if (rc != 0) bp_ui_toast("网盘暂不可用");
    if (bsp_lvgl_lock(500)) {
        bp_ui_list_reset_sel();
        bp_ui_goto(BP_PAGE_LIST);
        bsp_lvgl_unlock();
    }
}

static void open_list(bp_source_t source, const char *dir) {
    if (source == BP_SRC_DIR) {
        // 从根开始浏览网盘:重置目录层级栈。
        s_dir_depth = 0;
        strlcpy(s_dir_ids[0], "root", sizeof(s_dir_ids[0]));
    }
    open_list_at(source, dir, "root", 0, "");
}

static void list_activate(void) {
    bp_list_t *list = malloc(sizeof(bp_list_t));
    if (!list) return;
    bp_aliyun_list_snapshot(list);
    int fi = -1;
    bp_row_kind_t kind = BP_ROW_NONE;
    if (bsp_lvgl_lock(500)) {
        kind = bp_ui_list_row(&fi);
        bsp_lvgl_unlock();
    }
    switch (kind) {
        case BP_ROW_RETRY:
            open_list_at(list->req.source, list->req.dir, list->req.parent_file_id,
                         list->req.page, list->req.marker);
            break;
        case BP_ROW_NEXT:
            open_list_at(list->req.source, list->req.dir, list->req.parent_file_id,
                         list->req.page + 1, list->next_marker);
            break;
        case BP_ROW_PREV: {
            int prev = list->req.page - 1;
            // 超出历史记录(极深的翻页)时回到第一页。
            if (prev >= BP_PAGE_HISTORY)
                open_list_at(list->req.source, list->req.dir, list->req.parent_file_id, 0, "");
            else
                open_list_at(list->req.source, list->req.dir, list->req.parent_file_id,
                             prev, s_page_marker[prev]);
            break;
        }
        case BP_ROW_FILE: {
            const bp_file_t *f = &list->files[fi];
            if (f->is_dir) {
                char child[BP_PATH_MAX];
                if (bp_path_child(list->req.dir, f->name, child, sizeof(child))) {
                    // 压入目录栈:下一层的目录 file_id 即本目录。
                    if (s_dir_depth + 1 < BP_FOLDER_MAX) {
                        s_dir_depth++;
                        strlcpy(s_dir_ids[s_dir_depth], f->file_id, sizeof(s_dir_ids[s_dir_depth]));
                    }
                    open_list_at(BP_SRC_DIR, child, f->file_id, 0, "");
                } else {
                    bp_ui_toast("路径太长");
                }
            } else if (f->format == BP_FMT_UNKNOWN) {
                bp_ui_toast("暂不支持此格式");
            } else if (bp_player_play_list(list, fi) == 0) {
                go(BP_PAGE_PLAYER);
            }
            break;
        }
        default:
            break;
    }
    free(list);
}

static void list_back(void) {
    bp_list_t *list = malloc(sizeof(bp_list_t));
    if (!list) return;
    bp_aliyun_list_snapshot(list);
    char parent[BP_PATH_MAX];
    if (list->req.source == BP_SRC_DIR && bp_path_parent(list->req.dir, parent, sizeof(parent))) {
        // 返回上级:目录栈回退一层,用父目录的 file_id 重新列出。
        if (s_dir_depth > 0) s_dir_depth--;
        const char *pid = s_dir_ids[s_dir_depth];
        open_list_at(BP_SRC_DIR, parent, pid, 0, "");
    } else {
        go(BP_PAGE_SETTINGS);
    }
    free(list);
}

static void set_onboarding(bool on) {
    if (bsp_lvgl_lock(500)) {
        bp_ui_set_onboarding(on);
        bsp_lvgl_unlock();
    }
}

// 开机后第一次联网:在播放页且还没播过任何曲目时,从上次播放的目录接着播(每次开机只做一次)。
static bool s_autoplay_tried;

static void resume_hint(const char *line) {
    char folder[BP_FOLDER_MAX];
    if (bsp_lvgl_lock(500)) {
        if (line && bp_player_last_label(folder, sizeof(folder))) bp_ui_set_resume_hint(line, folder);
        else bp_ui_set_resume_hint(NULL, NULL);
        bsp_lvgl_unlock();
    }
}

static void autoplay_last(void) {
    if (s_autoplay_tried) return;
    s_autoplay_tried = true;
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    if (bp_ui_page() == BP_PAGE_PLAYER && !pi.name[0] && bp_player_last_label(NULL, 0)) {
        resume_hint("正在载入上次播放…");
        bp_player_play_last();
    } else {
        resume_hint(NULL);
    }
}

// 联网:未绑定网盘就进入第 2 步绑定;已绑定则从配网相关页面回到播放界面,并续播上次的目录。
static void on_wifi_up(void) {
    bp_page_t page = bp_ui_page();
    bool setup_page = page == BP_PAGE_PLAYER || page == BP_PAGE_WIFI ||
                      page == BP_PAGE_WIFI_AP || page == BP_PAGE_AUTH;
    if (bp_aliyun_state() == BP_BD_READY) {
        if (page == BP_PAGE_WIFI || page == BP_PAGE_WIFI_AP) {
            set_onboarding(false);
            bp_ui_toast("网络已连接");
            go(BP_PAGE_PLAYER);
        }
        autoplay_last();
        return;
    }
    resume_hint(NULL);   // 网盘未绑定:不会续播
    if (!setup_page || s_auth_declined) return;
    screen_off(false);
    s_last_activity_ms = now_ms();
    bp_aliyun_auth_start();
    set_onboarding(true);
    go(BP_PAGE_AUTH);
    bp_ui_toast("请用 USB 串口绑定阿里云盘");
}

// 绑定成功:在绑定页时直接打开“全部音频”,马上可以选歌。
static void on_authorized(void) {
    set_onboarding(false);
    if (bp_ui_page() != BP_PAGE_AUTH) return;
    screen_off(false);
    s_last_activity_ms = now_ms();
    bp_ui_toast("网盘绑定成功");
    open_list(BP_SRC_ALL_AUDIO, "/");
}

static void handle_input(const input_event_t *in) {
    if (in->btn == EVT_WIFI_UP) {
        on_wifi_up();
        return;
    }
    if (in->btn == EVT_AUTHORIZED) {
        on_authorized();
        return;
    }
    s_last_activity_ms = now_ms();

    // 唤醒手势整体吞掉:物理按键会先 PRESS,再 CLICK/LONG。
    if (s_wake_button == (int)in->btn) {
        if (in->event != BSP_BTN_PRESS) s_wake_button = -1;
        return;
    }
    if (g_bp.screen_off) {
        screen_off(false);
        if (in->event == BSP_BTN_PRESS) s_wake_button = (int)in->btn;
        return;
    }
    if (in->event == BSP_BTN_PRESS || in->event == BSP_BTN_DOUBLE) return;

    bool click = in->event == BSP_BTN_CLICK;
    bool lng = in->event == BSP_BTN_LONG;
    bool up = in->btn == BSP_BTN_UP, down = in->btn == BSP_BTN_DOWN, ok = in->btn == BSP_BTN_OK;
    bp_page_t page = bp_ui_page();

    switch (page) {
        case BP_PAGE_LIST:
            if ((up || down) && click) ui_move(up ? -1 : 1);
            else if ((up || down) && lng) ui_move(up ? -5 : 5);
            else if (ok && click) list_activate();
            else if (ok && lng) list_back();
            break;

        case BP_PAGE_PLAYER:
            if (ok && click) {
                bp_player_info_t pi;
                bp_player_get_info(&pi);
                // 还没有选过曲目:直接打开“全部音频”。
                if (!pi.name[0]) {
                    if (require_auth()) open_list(BP_SRC_ALL_AUDIO, "/");
                } else {
                    bp_player_toggle_pause();
                }
            } else if (ok && lng) {
                if (bsp_lvgl_lock(500)) {
                    bp_ui_set_selected(BP_PAGE_SETTINGS, 0);
                    bp_ui_goto(BP_PAGE_SETTINGS);
                    bsp_lvgl_unlock();
                }
            } else if ((up || down) && click) {
                bp_player_set_volume((uint8_t)bp_volume_step(g_bp.volume, up ? 10 : -10));
                if (bsp_lvgl_lock(500)) {
                    bp_ui_volume_feedback();
                    bsp_lvgl_unlock();
                }
            } else if ((up || down) && lng) {
                if (bsp_lvgl_lock(500)) {
                    bp_ui_reel_kick(up ? -1 : 1);
                    bsp_lvgl_unlock();
                }
                if (up) bp_player_prev();
                else bp_player_next();
            }
            break;

        case BP_PAGE_AUTH:
            if (ok && lng) {
                bp_aliyun_auth_cancel();
                s_auth_declined = true;
                set_onboarding(false);
                go(BP_PAGE_PLAYER);
            }
            break;

        case BP_PAGE_SETTINGS:
            if ((up || down) && click) ui_move(up ? -1 : 1);
            else if (ok && lng) go(BP_PAGE_PLAYER);
            else if (ok && click) {
                s_logout_armed = false;
                switch ((bp_menu_t)bp_ui_selected()) {
                    case BP_MENU_ALL_AUDIO:
                        if (require_auth()) open_list(BP_SRC_ALL_AUDIO, "/");
                        break;
                    case BP_MENU_BROWSE:
                        if (require_auth()) open_list(BP_SRC_DIR, "/");
                        break;
                    case BP_MENU_SKIN:
                        if (bsp_lvgl_lock(500)) {
                            bp_ui_set_selected(BP_PAGE_SKIN, g_bp.skin);
                            bp_ui_goto(BP_PAGE_SKIN);
                            bsp_lvgl_unlock();
                        }
                        break;
                    case BP_MENU_BRIGHTNESS: go(BP_PAGE_BRIGHTNESS); break;
                    case BP_MENU_WIFI: go(BP_PAGE_WIFI); break;
                    case BP_MENU_ACCOUNT: go(BP_PAGE_ACCOUNT); break;
                    case BP_MENU_ABOUT: go(BP_PAGE_ABOUT); break;
                    case BP_MENU_BACK: go(BP_PAGE_PLAYER); break;
                    default: break;
                }
            }
            break;

        case BP_PAGE_SKIN:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            else if ((up || down) && click) ui_move(up ? -1 : 1);
            else if (ok && click) {
                set_skin((bp_skin_t)bp_ui_selected());
                go(BP_PAGE_SKIN);   // 刷新“使用中”标记
            }
            break;

        case BP_PAGE_ACCOUNT:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            else if (ok && click) {
                if (bp_aliyun_state() != BP_BD_READY) {
                    require_auth();
                } else if (!s_logout_armed) {
                    s_logout_armed = true;
                    if (bsp_lvgl_lock(500)) { bp_ui_account_arm(true); bsp_lvgl_unlock(); }
                } else {
                    s_logout_armed = false;
                    bp_player_stop();
                    bp_ui_toast(bp_aliyun_logout() == 0 ? "已退出网盘账号" : "退出失败");
                    if (bsp_lvgl_lock(500)) { bp_ui_account_arm(false); bsp_lvgl_unlock(); }
                }
            } else if ((up || down) && click && s_logout_armed) {
                s_logout_armed = false;
                if (bsp_lvgl_lock(500)) { bp_ui_account_arm(false); bsp_lvgl_unlock(); }
            }
            break;

        case BP_PAGE_WIFI:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            else if (ok && click) start_web_config();
            break;

        case BP_PAGE_WIFI_AP:
            if (ok && lng) {
                bp_wifi_config_stop();
                go(BP_PAGE_WIFI);
            }
            break;

        case BP_PAGE_ABOUT:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            break;

        case BP_PAGE_BRIGHTNESS:
            if (ok && lng) {
                go(BP_PAGE_SETTINGS);
            } else if ((up || down) && click) {
                set_brightness(bp_brightness_step(g_bp.brightness, up ? 1 : -1));
                go(BP_PAGE_BRIGHTNESS);   // 立即刷新竖条与档位
            }
            break;

        default:
            break;
    }
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t in;
    for (;;)
        if (xQueueReceive(s_input_queue, &in, portMAX_DELAY) == pdTRUE) handle_input(&in);
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready) return;
    const input_event_t in = {.btn = btn, .event = ev};
    (void)xQueueSend(s_input_queue, &in, 0);
}

int bp_test_key(int button, int kind) {
    if (!s_input_ready || button < 0 || button > 2 || kind < 0 || kind > 2) return -1;
    static const bsp_btn_t buttons[] = {BSP_BTN_UP, BSP_BTN_OK, BSP_BTN_DOWN};
    static const bsp_btn_ev_t kinds[] = {BSP_BTN_CLICK, BSP_BTN_LONG, BSP_BTN_DOUBLE};
    const input_event_t in = {.btn = buttons[button], .event = kinds[kind]};
    return xQueueSend(s_input_queue, &in, pdMS_TO_TICKS(100)) == pdTRUE ? 0 : -1;
}

int bp_test_page(void) { return bp_ui_page(); }

// ---- 定时器 -----------------------------------------------------------------------
static void idle_timer_cb(void *arg) {
    (void)arg;
    // 自动熄屏已按用户要求取消:屏幕保持常亮,不随空闲超时关闭。
    // 如需恢复,重新启用 IDLE_TIMEOUT_MS 判断并调用 screen_off(true) 即可。
}

static void heartbeat_cb(void *arg) {
    (void)arg;
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    ESP_LOGI(TAG, "[HB] free=%u min=%u largest=%u wifi=%d bd=%d play=%d pos=%lu buf=%u%%",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), g_bp.wifi_up,
             bp_aliyun_state(), pi.state, (unsigned long)pi.pos_ms, pi.buffer_pct);
}

// UI 看门狗:taskLVGL 若因堆耗尽等原因陷入 while(1)(实测 LVGL 字形缓冲分配失败
// 会触发 LV_ASSERT_HANDLER=while(1)),200ms 的 UI 刷新打点就会停止。连续约 12s
// 无刷新即重启,避免屏幕永久停在"正在读取"只能拔电。
static void ui_wdt_cb(void *arg) {
    (void)arg;
    static int dead_ticks = 0;
    int64_t age = bp_ui_ms_since_refresh();
    if (age > 12000) {
        if (++dead_ticks >= 6) {
            ESP_LOGE(TAG, "UI unresponsive for %lld ms; restarting", (long long)age);
            esp_restart();
        }
    } else {
        dead_ticks = 0;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Aliyunpan Pocket Player v" BP_APP_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed");
        return;
    }
    bsp_battery_init();
    bp_aliyun_init();
    bp_player_init();
    load_ui_prefs();   // 皮肤与亮度要在界面创建前确定

    if (bsp_lvgl_lock(1000)) {
        bp_ui_init();
        bsp_lvgl_unlock();
    }
    apply_backlight();
    s_last_activity_ms = now_ms();

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (s_input_queue && xTaskCreate(input_task, "bp_input", 6144, NULL, 5, NULL) == pdPASS &&
        bsp_button_init(on_key, NULL) == ESP_OK) {
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "button init failed");
    }

    const esp_timer_create_args_t idle_args = {.callback = idle_timer_cb, .name = "bp_idle"};
    esp_timer_handle_t idle_timer;
    if (esp_timer_create(&idle_args, &idle_timer) == ESP_OK)
        esp_timer_start_periodic(idle_timer, 1000 * 1000);
    const esp_timer_create_args_t hb_args = {.callback = heartbeat_cb, .name = "bp_hb"};
    esp_timer_handle_t hb_timer;
    if (esp_timer_create(&hb_args, &hb_timer) == ESP_OK)
        esp_timer_start_periodic(hb_timer, 30 * 1000 * 1000);
    const esp_timer_create_args_t uiwdt_args = {.callback = ui_wdt_cb, .name = "bp_uiwdt"};
    esp_timer_handle_t uiwdt_timer;
    if (esp_timer_create(&uiwdt_args, &uiwdt_timer) == ESP_OK)
        esp_timer_start_periodic(uiwdt_timer, 2 * 1000 * 1000);

    bp_wifi_init(on_wifi_event);
    if (bp_wifi_saved_count() == 0) {
        // 首次使用:直接开启配网热点,手机扫屏幕二维码即可配网。
        ESP_LOGI(TAG, "no saved Wi-Fi; starting web provisioning hotspot");
        bp_wifi_config_start();
        set_onboarding(true);
        go(BP_PAGE_WIFI_AP);
    } else {
        // 已绑定网盘且有上次播放记录:联网前先在播放页提示即将续播,免得以为没反应。
        if (bp_aliyun_state() == BP_BD_READY) resume_hint("正在连接网络…");
        bp_wifi_start_station();
    }
    bp_console_start();
}
