// main/bp_ui.c —— 阿里云盘随身听界面(240x320 竖屏,圆角屏)。
//
// 常用页面(播放、列表、设置)在 bp_ui_init 建好,切页只切换 HIDDEN;不常用的页面
// (绑定、配网、无线网络、账号、亮度、关于、播放界面选择)进入时创建、离开时删除,
// 播放页也只建当前皮肤 —— 界面全部常驻约 49 KB,会挤掉播放时解码器需要的内存。
// 状态栏与提示条放在 lv_layer_top(),各页共用。
// 动态内容由 200 ms 的 lv_timer 刷新(运行在 LVGL 任务内,已持有锁)。
// 中文用 bp_font_16(GB2312 全集,显示任意网盘文件名)与 bp_font_24(界面固定文字子集);
// 图标用 Montserrat 内置符号,中文字体里没有这些码位。
#include "bp_app.h"
#include "bp_ui_reel.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(bp_font_16);
LV_FONT_DECLARE(bp_font_24);

#define COL_BG       0x0B1220
#define COL_SURFACE  0x16213A
#define COL_SURFACE2 0x22314F
// 主题色:紫色(原绿色 0x20E47C,按用户要求改为紫色)。
#define COL_ACCENT   0x9C27B0
#define COL_ON_ACC   0x2A0A33
#define COL_TEXT     0xEEF2F8
#define COL_MUTED    0x8A97B0
#define COL_DANGER   0xFF6B6B

#define LIST_ROWS 7
#define LIST_ROW_H 32
#define LIST_TOP 70
#define MENU_ROWS 5          // 设置菜单一屏显示的行数
#define TOAST_MS 2200
#define ANIM_MS 50           // 磁带界面动画周期

static lv_obj_t *s_pages[BP_PAGE_COUNT];
static bp_page_t s_page = BP_PAGE_PLAYER;
static int s_sel[BP_PAGE_COUNT];

// 顶层
static lv_obj_t *s_st_wifi, *s_st_play, *s_st_batt, *s_toast, *s_toast_label;
static portMUX_TYPE s_toast_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_toast_text[64];
static bool s_toast_pending;
static int64_t s_toast_until;


// 列表页
static lv_obj_t *s_list_title, *s_list_sub, *s_list_msg;
static lv_obj_t *s_rows[LIST_ROWS], *s_row_icons[LIST_ROWS], *s_row_texts[LIST_ROWS];
static bp_list_t s_list;     // 最近一次快照
static int s_list_rows;      // 虚拟行数(文件 + 上/下一页)

// 播放页:经典与磁带(含人物磁带)界面放在同一页里,按 g_bp.skin 显示其一。

const char *const BP_SKIN_NAMES[BP_SKIN_COUNT] = {"经典", "磁带", "布鲁伊", "小猪佩奇"};
extern const lv_image_dsc_t bp_disc_bluey_thumb, bp_disc_bingo_thumb, bp_disc_peppa_thumb;
static lv_obj_t *s_pl_classic, *s_pl_reel;
static int64_t s_anim_last;
// 最近一次完整跑完 200ms UI 刷新的时间(esp_timer 微秒)。0=尚未完成过一次。
static int64_t s_last_refresh_us;
static int s_built_skin = -1;
static bool s_hint_on;                       // 开机续播提示
static char s_hint_line[48], s_hint_folder[BP_FOLDER_MAX];
static bool s_onboarding;
static lv_obj_t *s_arc, *s_pl_time, *s_pl_total, *s_pl_state, *s_pl_name, *s_pl_meta;
static lv_obj_t *s_pl_vol_bar, *s_pl_icon;

// 授权页
static lv_obj_t *s_auth_qr_box, *s_auth_qr, *s_auth_code, *s_auth_hint, *s_auth_title;
static char s_auth_shown[96];

// 设置/信息页
static lv_obj_t *s_set_items[MENU_ROWS], *s_set_icons[MENU_ROWS], *s_set_texts[MENU_ROWS];
static lv_obj_t *s_set_vals[MENU_ROWS], *s_set_thumb;

// 播放界面选择页
static lv_obj_t *s_skin_card[BP_SKIN_COUNT], *s_skin_tag[BP_SKIN_COUNT], *s_skin_name[BP_SKIN_COUNT];
static lv_obj_t *s_wifi_info, *s_wifi_action, *s_acct_state, *s_acct_action, *s_acct_hint;
static bool s_acct_armed;

// 亮度页
static lv_obj_t *s_bri_bars[BP_BRIGHTNESS_LEVELS], *s_bri_value;

// 配网热点页
static lv_obj_t *s_ap_qr, *s_ap_ssid, *s_ap_steps, *s_ap_title;
static char s_ap_shown[33];

// ---- 小工具 ---------------------------------------------------------------------
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

// 单行显示:DOTS 模式只有在高度固定为一行时才不换行,过长部分以省略号结尾。
static void one_line(lv_obj_t *l, int32_t w) {
    lv_obj_set_size(l, w, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

static void set_text_if(lv_obj_t *l, const char *text) {
    const char *old = lv_label_get_text(l);
    if (!old || strcmp(old, text)) lv_label_set_text(l, text);
}

static lv_obj_t *page(void) {
    lv_obj_t *p = box(lv_screen_active(), 0, 0, 240, 320, COL_BG, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *page_title(lv_obj_t *p, const char *text) {
    lv_obj_t *t = label(p, &bp_font_24, COL_ACCENT, text);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 32);
    return t;
}

// 胶囊菜单项:选中时品牌绿底深色字。
static void pill(lv_obj_t *parent, int y, const char *icon, const char *text,
                 lv_obj_t **item, lv_obj_t **icon_l, lv_obj_t **text_l) {
    *item = box(parent, 20, y, 200, 40, COL_SURFACE, 20);
    *icon_l = label(*item, &lv_font_montserrat_14, COL_ACCENT, icon);
    lv_obj_align(*icon_l, LV_ALIGN_LEFT_MID, 18, 0);
    *text_l = label(*item, &bp_font_16, COL_TEXT, text);
    lv_obj_align(*text_l, LV_ALIGN_LEFT_MID, 46, 0);
}

static void pill_select(lv_obj_t *item, lv_obj_t *icon, lv_obj_t *text, bool on) {
    lv_obj_set_style_bg_color(item, lv_color_hex(on ? COL_ACCENT : COL_SURFACE), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(on ? COL_ON_ACC : COL_ACCENT), 0);
    lv_obj_set_style_text_color(text, lv_color_hex(on ? COL_ON_ACC : COL_TEXT), 0);
}

static const char *state_text(const bp_player_info_t *pi, char *buf, size_t cap) {
    switch (pi->state) {
        case BP_PLAY_RESOLVING: return "正在连接网盘…";
        case BP_PLAY_BUFFERING: snprintf(buf, cap, "缓冲中 %u%%", pi->buffer_pct); return buf;
        case BP_PLAY_PLAYING:   return "播放中";
        case BP_PLAY_PAUSED:    return "已暂停";
        case BP_PLAY_ERROR:     return pi->error ? pi->error : "播放失败";
        default:                return "未在播放";
    }
}

// ---- 构建页面 -------------------------------------------------------------------
static void build_top_layer(void) {
    // 状态栏:左 Wi-Fi、中 播放状态、右 电量。左右各内缩 32px,避开 30px 圆角。
    s_st_wifi = label(lv_layer_top(), &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_st_wifi, LV_ALIGN_TOP_LEFT, 32, 8);
    s_st_play = label(lv_layer_top(), &lv_font_montserrat_14, COL_ACCENT, "");
    lv_obj_align(s_st_play, LV_ALIGN_TOP_MID, 0, 8);
    s_st_batt = label(lv_layer_top(), &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_st_batt, LV_ALIGN_TOP_RIGHT, -32, 8);

    s_toast = box(lv_layer_top(), 24, 262, 192, 36, COL_SURFACE2, 18);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    s_toast_label = label(s_toast, &bp_font_16, COL_TEXT, "");
    one_line(s_toast_label, 176);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_toast_label);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

static void build_list(void) {
    lv_obj_t *p = s_pages[BP_PAGE_LIST] = page();
    s_list_title = label(p, &bp_font_16, COL_ACCENT, "");
    lv_obj_set_width(s_list_title, 180);
    lv_label_set_long_mode(s_list_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);   // 当前目录名过长时滚动
    lv_obj_set_style_text_align(s_list_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_list_title, LV_ALIGN_TOP_MID, 0, 30);
    s_list_sub = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_align(s_list_sub, LV_ALIGN_TOP_MID, 0, 48);
    for (int i = 0; i < LIST_ROWS; i++) {
        s_rows[i] = box(p, 12, LIST_TOP + i * LIST_ROW_H, 216, LIST_ROW_H - 4, COL_BG, 10);
        s_row_icons[i] = label(s_rows[i], &lv_font_montserrat_14, COL_MUTED, "");
        lv_obj_align(s_row_icons[i], LV_ALIGN_LEFT_MID, 10, 0);
        s_row_texts[i] = label(s_rows[i], &bp_font_16, COL_TEXT, "");
        one_line(s_row_texts[i], 176);
        lv_obj_align(s_row_texts[i], LV_ALIGN_LEFT_MID, 32, 0);
    }
    s_list_msg = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_set_width(s_list_msg, 200);
    lv_obj_set_style_text_align(s_list_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_list_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_list_msg, LV_ALIGN_CENTER, 0, 0);
}

static void build_classic(lv_obj_t *p) {
    s_arc = lv_arc_create(p);
    lv_obj_set_size(s_arc, 176, 176);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 34);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    s_pl_icon = label(s_arc, &lv_font_montserrat_20, COL_ACCENT, LV_SYMBOL_PLAY);
    lv_obj_align(s_pl_icon, LV_ALIGN_CENTER, 0, -46);
    s_pl_time = label(s_arc, &lv_font_montserrat_28, COL_TEXT, "0:00");
    lv_obj_align(s_pl_time, LV_ALIGN_CENTER, 0, -10);
    s_pl_total = label(s_arc, &lv_font_montserrat_14, COL_MUTED, "/ --:--");
    lv_obj_align(s_pl_total, LV_ALIGN_CENTER, 0, 16);
    s_pl_state = label(s_arc, &bp_font_16, COL_MUTED, "");
    one_line(s_pl_state, 130);
    lv_obj_set_style_text_align(s_pl_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pl_state, LV_ALIGN_CENTER, 0, 42);

    s_pl_name = label(p, &bp_font_16, COL_TEXT, "");
    lv_obj_set_width(s_pl_name, 200);
    lv_obj_set_style_text_align(s_pl_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pl_name, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(s_pl_name, LV_ALIGN_TOP_MID, 0, 220);
    s_pl_meta = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_align(s_pl_meta, LV_ALIGN_TOP_MID, 0, 244);

    lv_obj_t *vol = label(p, &lv_font_montserrat_14, COL_MUTED, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_pos(vol, 46, 276);
    s_pl_vol_bar = lv_bar_create(p);
    lv_obj_set_size(s_pl_vol_bar, 120, 6);
    lv_obj_set_pos(s_pl_vol_bar, 74, 282);
    lv_bar_set_range(s_pl_vol_bar, 0, 100);
    lv_obj_set_style_bg_color(s_pl_vol_bar, lv_color_hex(COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pl_vol_bar, lv_color_hex(COL_MUTED), LV_PART_INDICATOR);
}

static void build_player(void) {
    lv_obj_t *page_obj = s_pages[BP_PAGE_PLAYER] = page();
    s_pl_reel = box(page_obj, 0, 0, 240, 320, COL_BG, 0);
    s_pl_classic = box(page_obj, 0, 0, 240, 320, COL_BG, 0);
}

static void build_auth(void) {
    lv_obj_t *p = s_pages[BP_PAGE_AUTH] = page();
    s_auth_title = page_title(p, s_onboarding ? "第2步 绑定网盘" : "绑定阿里云盘");
    s_auth_qr_box = box(p, 44, 72, 152, 152, 0xFFFFFF, 12);
    s_auth_qr = lv_qrcode_create(s_auth_qr_box);
    // 120px 二维码位图约 28.8KB(136px 要 36KB):授权页无播放、堆较紧张,减小位图
    // 尺寸可避免 LVGL 渲染标题字形时内存不足导致缺字/残影。
    lv_qrcode_set_size(s_auth_qr, 120);
    lv_qrcode_set_dark_color(s_auth_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_auth_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(s_auth_qr);
    s_auth_code = label(p, &bp_font_16, COL_TEXT, "");   // 二维码下方中文提示必须用中文字体
    lv_obj_align(s_auth_code, LV_ALIGN_TOP_MID, 0, 232);
    s_auth_hint = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_set_width(s_auth_hint, 196);
    lv_obj_set_style_text_align(s_auth_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_auth_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_auth_hint, LV_ALIGN_TOP_MID, 0, 262);
}

static const char *const MENU_ICONS[BP_MENU_COUNT] = {
    LV_SYMBOL_AUDIO, LV_SYMBOL_DIRECTORY, LV_SYMBOL_IMAGE, LV_SYMBOL_EYE_OPEN,
    LV_SYMBOL_WIFI, LV_SYMBOL_DRIVE, LV_SYMBOL_LIST, LV_SYMBOL_LEFT,
};
static const char *const MENU_TEXTS[BP_MENU_COUNT] = {
    "全部音频", "浏览网盘", "播放界面", "屏幕亮度", "无线网络", "网盘账号", "关于与按键", "返回",
};

static void build_settings(void) {
    lv_obj_t *p = s_pages[BP_PAGE_SETTINGS] = page();
    page_title(p, "设置");
    for (int i = 0; i < MENU_ROWS; i++) {
        pill(p, 72 + i * 46, "", "", &s_set_items[i], &s_set_icons[i], &s_set_texts[i]);
        s_set_vals[i] = label(s_set_items[i], &bp_font_16, COL_MUTED, "");
        lv_obj_align(s_set_vals[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }
    // 滚动条:7 项只显示 5 项时提示还有更多。
    lv_obj_t *track = box(p, 227, 74, 3, 222, COL_SURFACE2, 2);
    s_set_thumb = box(track, 0, 0, 3, 222 * MENU_ROWS / BP_MENU_COUNT, COL_ACCENT, 2);

}

// 播放界面选择:两张卡片,上/下切换,OK 应用。
// 皮肤卡片缩略图:两个磁带轮 + 走带线 + 两条文字条;img 为空时画普通转轮。
static void tape_thumb(lv_obj_t *c, const lv_image_dsc_t *l, const lv_image_dsc_t *r) {
    for (int i = 0; i < 2; i++) {
        const lv_image_dsc_t *img = i ? r : l;
        if (img) {
            lv_obj_t *o = lv_image_create(c);
            lv_image_set_src(o, img);
            lv_obj_set_pos(o, i ? 50 : 6, 6);
            continue;
        }
        lv_obj_t *reel = box(c, i ? 52 : 8, 8, 32, 32, 0xF3E5F5, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(reel, 1, 0);
        lv_obj_set_style_border_color(reel, lv_color_hex(0x2A0A33), 0);
        box(reel, 11, 11, 10, 10, i ? 0x7B1FA2 : 0x2A0A33, LV_RADIUS_CIRCLE);
    }
    box(c, 10, 50, 72, 1, 0x2A0A33, 0);
    box(c, 10, 58, 44, 5, 0x2A0A33, 2);
    box(c, 10, 67, 30, 4, 0x2A0A33, 2);
}

static void build_skin(void) {
    lv_obj_t *p = s_pages[BP_PAGE_SKIN] = page();
    page_title(p, "播放界面");
    // 磁带皮肤背景由绿色改为紫色(与主题色一致)。
    static const uint32_t bg[BP_SKIN_COUNT] = {COL_BG, 0x9C27B0, 0x5CB8EC, 0xF58FB3};
    for (int i = 0; i < BP_SKIN_COUNT; i++) {
        int x = i % 2 ? 126 : 22, y = i / 2 ? 166 : 60;
        lv_obj_t *c = s_skin_card[i] = box(p, x, y, 92, 80, bg[i], 14);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0x2C3A55), 0);
        s_skin_name[i] = label(p, &bp_font_16, COL_TEXT, BP_SKIN_NAMES[i]);
        lv_obj_set_width(s_skin_name[i], 92);
        lv_obj_set_style_text_align(s_skin_name[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_skin_name[i], x, y + 81);
        switch (i) {
            case BP_SKIN_CLASSIC: {   // 进度环 + 播放三角 + 文本条
                lv_obj_t *ring = lv_arc_create(c);
                lv_obj_set_size(ring, 42, 42);
                lv_obj_set_pos(ring, 25, 4);
                lv_arc_set_rotation(ring, 270);
                lv_arc_set_bg_angles(ring, 0, 360);
                lv_arc_set_value(ring, 60);
                lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
                lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
                lv_obj_set_style_arc_width(ring, 4, LV_PART_INDICATOR);
                lv_obj_set_style_arc_color(ring, lv_color_hex(COL_SURFACE2), LV_PART_MAIN);
                lv_obj_set_style_arc_color(ring, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
                lv_obj_t *tri = label(ring, &lv_font_montserrat_14, COL_ACCENT, LV_SYMBOL_PLAY);
                lv_obj_center(tri);
                box(c, 20, 54, 50, 5, COL_TEXT, 2);
                box(c, 28, 65, 34, 4, COL_MUTED, 2);
                break;
            }
            case BP_SKIN_REEL: tape_thumb(c, NULL, NULL); break;
            case BP_SKIN_BLUEY: tape_thumb(c, &bp_disc_bluey_thumb, &bp_disc_bingo_thumb); break;
            default: tape_thumb(c, &bp_disc_peppa_thumb, &bp_disc_peppa_thumb); break;
        }
        // 使用中:右下角的勾。
        s_skin_tag[i] = box(c, 68, 56, 18, 18, 0x2A0A33, LV_RADIUS_CIRCLE);
        lv_obj_t *ok = label(s_skin_tag[i], &lv_font_montserrat_14, 0xFFFFFF, LV_SYMBOL_OK);
        lv_obj_center(ok);
    }
    lv_obj_t *hint = label(p, &bp_font_16, COL_MUTED, "上/下 选择 · OK 应用");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 278);
}

static lv_obj_t *info_label(lv_obj_t *p, int y) {
    lv_obj_t *l = label(p, &bp_font_16, COL_TEXT, "");
    lv_obj_set_width(l, 196);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(l, 6, 0);
    lv_obj_set_pos(l, 22, y);
    return l;
}

static void build_wifi(void) {
    lv_obj_t *p = s_pages[BP_PAGE_WIFI] = page();
    page_title(p, "无线网络");
    s_wifi_info = info_label(p, 76);
    lv_obj_t *item, *icon;
    pill(p, 240, LV_SYMBOL_WIFI, "开启配网热点", &item, &icon, &s_wifi_action);
    pill_select(item, icon, s_wifi_action, true);
}

static void build_wifi_ap(void) {
    lv_obj_t *p = s_pages[BP_PAGE_WIFI_AP] = page();
    s_ap_title = page_title(p, s_onboarding ? "第1步 连接网络" : "网页配网");
    lv_obj_t *qr_box = box(p, 60, 70, 120, 120, 0xFFFFFF, 10);
    s_ap_qr = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(s_ap_qr, 108);
    lv_qrcode_set_dark_color(s_ap_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_ap_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(s_ap_qr);
    s_ap_ssid = label(p, &lv_font_montserrat_14, COL_ACCENT, "");
    lv_obj_align(s_ap_ssid, LV_ALIGN_TOP_MID, 0, 196);
    s_ap_steps = info_label(p, 218);
    lv_obj_set_style_text_line_space(s_ap_steps, 2, 0);
    lv_obj_set_style_text_color(s_ap_steps, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text(s_ap_steps, "1. 手机扫码连接此热点\n"
                                  "2. 在弹出的网页选网络、填密码\n"
                                  "未弹出请访问 192.168.4.1");
}

static void build_account(void) {
    lv_obj_t *item, *icon;
    lv_obj_t *p = s_pages[BP_PAGE_ACCOUNT] = page();
    page_title(p, "网盘账号");
    s_acct_state = info_label(p, 80);
    pill(p, 172, LV_SYMBOL_REFRESH, "", &item, &icon, &s_acct_action);
    pill_select(item, icon, s_acct_action, true);
    s_acct_hint = info_label(p, 224);
    lv_obj_set_style_text_color(s_acct_hint, lv_color_hex(COL_MUTED), 0);
}

// 亮度:5 根由矮到高的竖条,点亮的根数即档位。
static void build_brightness(void) {
    lv_obj_t *p = s_pages[BP_PAGE_BRIGHTNESS] = page();
    page_title(p, "屏幕亮度");
    for (int i = 0; i < BP_BRIGHTNESS_LEVELS; i++) {
        int h = 28 + i * 18;
        s_bri_bars[i] = box(p, 28 + i * 38, 210 - h, 28, h, COL_SURFACE2, 8);
    }
    s_bri_value = label(p, &lv_font_montserrat_28, COL_TEXT, "");
    lv_obj_align(s_bri_value, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_t *bri_hint = label(p, &bp_font_16, COL_MUTED, "上/下 调节 · 长按 OK 返回");
    lv_obj_align(bri_hint, LV_ALIGN_TOP_MID, 0, 266);
}

static void build_about(void) {
    lv_obj_t *p = s_pages[BP_PAGE_ABOUT] = page();
    page_title(p, "关于与按键");
    // 两栏:左按键(品牌绿)右说明,每项一行;说明超宽时省略而不换行。
    static const char *keys[][2] = {
        {"OK", "播放 · 暂停"},
        {"长按 OK", "进入设置"},
        {"上 / 下", "调节音量"},
        {"长按上/下", "上一首/下一首"},
        {"菜单里 OK", "进入 · 确认"},
        {"菜单长按OK", "返回上一层"},
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        int y = 72 + (int)i * 28;
        lv_obj_t *k = label(p, &bp_font_16, COL_ACCENT, keys[i][0]);
        lv_obj_set_pos(k, 22, y);
        lv_obj_t *d = label(p, &bp_font_16, COL_TEXT, keys[i][1]);
        one_line(d, 104);
        lv_obj_set_pos(d, 116, y);
    }
    lv_obj_t *formats = label(p, &bp_font_16, COL_MUTED, "支持 MP3 · M4A · AAC · WAV");
    lv_obj_align(formats, LV_ALIGN_TOP_MID, 0, 248);
    lv_obj_t *ver = label(p, &bp_font_16, COL_MUTED, "阿里云盘随身听 v" BP_APP_VERSION);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 270);
}

typedef void (*page_builder_t)(void);
static const page_builder_t LAZY_BUILDERS[BP_PAGE_COUNT] = {
    [BP_PAGE_AUTH] = build_auth, [BP_PAGE_WIFI] = build_wifi, [BP_PAGE_WIFI_AP] = build_wifi_ap,
    [BP_PAGE_ACCOUNT] = build_account, [BP_PAGE_ABOUT] = build_about,
    [BP_PAGE_BRIGHTNESS] = build_brightness, [BP_PAGE_SKIN] = build_skin,
};

// ---- 刷新 -------------------------------------------------------------------------
static void refresh_status(void) {
    // Wi-Fi:已连接为灰色图标,未连接为红色图标。
    set_text_if(s_st_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_st_wifi, lv_color_hex(g_bp.wifi_up ? COL_MUTED : COL_DANGER), 0);

    bp_player_info_t pi;
    bp_player_get_info(&pi);
    set_text_if(s_st_play, pi.state == BP_PLAY_PLAYING ? LV_SYMBOL_PLAY :
                           pi.state == BP_PLAY_PAUSED ? LV_SYMBOL_PAUSE : "");

    char buf[24];
    int soc = bsp_battery_soc();
    const char *batt = soc > 80 ? LV_SYMBOL_BATTERY_FULL : soc > 55 ? LV_SYMBOL_BATTERY_3
                     : soc > 30 ? LV_SYMBOL_BATTERY_2 : soc > 10 ? LV_SYMBOL_BATTERY_1
                     : LV_SYMBOL_BATTERY_EMPTY;
    if (soc >= 0) snprintf(buf, sizeof(buf), "%d%% %s", soc, batt);
    else buf[0] = 0;
    set_text_if(s_st_batt, buf);
    lv_obj_set_style_text_color(s_st_batt, lv_color_hex(soc >= 0 && soc <= 10 ? COL_DANGER : COL_MUTED), 0);
}

static void refresh_toast(void) {
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_toast_mux);
    bool pending = s_toast_pending;
    char text[sizeof(s_toast_text)];
    if (pending) {
        memcpy(text, s_toast_text, sizeof(text));
        s_toast_pending = false;
    }
    portEXIT_CRITICAL(&s_toast_mux);
    if (pending) {
        lv_label_set_text(s_toast_label, text);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = now + TOAST_MS;
    } else if (s_toast_until && now > s_toast_until) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}

static bool list_changed(const bp_list_t *a, const bp_list_t *b) {
    return a->status != b->status || a->count != b->count || a->has_more != b->has_more ||
           a->req.page != b->req.page || strcmp(a->req.marker, b->req.marker) != 0 ||
           a->req.source != b->req.source ||
           strcmp(a->req.dir, b->req.dir) ||
           (a->count && strcmp(a->files[0].file_id, b->files[0].file_id) != 0);
}

// 虚拟行:[上一页] 文件... [下一页]
static bp_row_kind_t row_kind(int row, int *file_index) {
    int r = row;
    bool prev = s_list.req.page > 0;
    if (prev) {
        if (r == 0) return BP_ROW_PREV;
        r--;
    }
    if (r < s_list.count) {
        if (file_index) *file_index = r;
        return BP_ROW_FILE;
    }
    if (r == s_list.count && s_list.has_more) return BP_ROW_NEXT;
    return BP_ROW_NONE;
}

static void refresh_list(void) {
    static bp_list_t snap;   // 约 1.7 KiB,不放在 LVGL 任务栈上
    bp_aliyun_list_snapshot(&snap);
    if (list_changed(&snap, &s_list)) {
        s_list = snap;
        s_list_rows = s_list.count + (s_list.req.page > 0) + (s_list.has_more ? 1 : 0);
        if (s_sel[BP_PAGE_LIST] >= s_list_rows) s_sel[BP_PAGE_LIST] = 0;
    }
    char buf[64];
    if (s_list.req.source == BP_SRC_ALL_AUDIO) set_text_if(s_list_title, "全部音频");
    else set_text_if(s_list_title, !strcmp(s_list.req.dir, "/") ? "我的网盘" : bp_path_basename(s_list.req.dir));
    snprintf(buf, sizeof(buf), "第 %d 页", s_list.req.page + 1);
    set_text_if(s_list_sub, buf);

    const char *msg = NULL;
    if (s_list.status == 1 || s_list.status == 0) msg = "正在读取网盘…";
    else if (s_list.status < 0) msg = "读取失败\n按 OK 重试";
    else if (s_list_rows == 0) msg = s_list.req.source == BP_SRC_ALL_AUDIO ?
                                     "网盘里还没有 MP3/WAV\n按 OK 刷新" : "这里没有可播放的音频";
    if (msg) {
        set_text_if(s_list_msg, msg);
        lv_obj_remove_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
    }

    int sel = s_sel[BP_PAGE_LIST];
    int first = sel - LIST_ROWS / 2;
    if (first > s_list_rows - LIST_ROWS) first = s_list_rows - LIST_ROWS;
    if (first < 0) first = 0;
    for (int i = 0; i < LIST_ROWS; i++) {
        int row = first + i;
        int fi = -1;
        bp_row_kind_t kind = msg ? BP_ROW_NONE : row_kind(row, &fi);
        if (kind == BP_ROW_NONE) {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        bool on = row == sel;
        const char *icon = LV_SYMBOL_FILE, *text = "";
        uint32_t icon_col = COL_MUTED, text_col = COL_TEXT;
        if (kind == BP_ROW_PREV) { icon = LV_SYMBOL_LEFT; text = "上一页"; text_col = COL_MUTED; }
        else if (kind == BP_ROW_NEXT) { icon = LV_SYMBOL_RIGHT; text = "下一页"; text_col = COL_MUTED; }
        else {
            const bp_file_t *f = &s_list.files[fi];
            text = f->name;
            if (f->is_dir) { icon = LV_SYMBOL_DIRECTORY; icon_col = COL_ACCENT; }
            else if (f->format != BP_FMT_UNKNOWN) { icon = LV_SYMBOL_AUDIO; icon_col = COL_ACCENT; }
            else text_col = COL_MUTED;   // 不支持的格式置灰
        }
        set_text_if(s_row_icons[i], icon);
        set_text_if(s_row_texts[i], text);
        lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(on ? COL_SURFACE2 : COL_BG), 0);
        lv_obj_set_style_border_color(s_rows[i], lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(s_rows[i], on ? 1 : 0, 0);
        lv_obj_set_style_text_color(s_row_icons[i], lv_color_hex(icon_col), 0);
        lv_obj_set_style_text_color(s_row_texts[i], lv_color_hex(text_col), 0);
        // 选中行文字过长时滚动显示,其余行省略号。
        lv_label_long_mode_t mode = on ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS;
        if (lv_label_get_long_mode(s_row_texts[i]) != mode) lv_label_set_long_mode(s_row_texts[i], mode);
    }
}

static void refresh_player(const bp_player_info_t *ppi) {
    bp_player_info_t pi = *ppi;
    char buf[48], t[16];
    bp_format_time(pi.pos_ms, t, sizeof(t));
    set_text_if(s_pl_time, t);
    if (pi.total_ms) {
        bp_format_time(pi.total_ms, t, sizeof(t));
        snprintf(buf, sizeof(buf), "/ %s", t);
    } else {
        strlcpy(buf, "/ --:--", sizeof(buf));
    }
    set_text_if(s_pl_total, buf);
    uint32_t prog = pi.total_ms ? (uint32_t)((uint64_t)pi.pos_ms * 1000 / pi.total_ms) : 0;
    lv_arc_set_value(s_arc, (int32_t)(prog > 1000 ? 1000 : prog));
    set_text_if(s_pl_state, state_text(&pi, buf, sizeof(buf)));
    lv_obj_set_style_text_color(s_pl_state, lv_color_hex(pi.state == BP_PLAY_ERROR ? COL_DANGER : COL_MUTED), 0);
    set_text_if(s_pl_icon, pi.state == BP_PLAY_PAUSED ? LV_SYMBOL_PAUSE :
                           pi.state == BP_PLAY_ERROR ? LV_SYMBOL_WARNING : LV_SYMBOL_PLAY);
    set_text_if(s_pl_name, pi.name[0] ? pi.name : "未在播放");
    if (s_hint_on) set_text_if(s_pl_state, pi.folder);   // 续播提示:状态行显示目录
    if (pi.count && pi.kbps) snprintf(buf, sizeof(buf), "%d / %d · %lu kbps", pi.index + 1, pi.count,
                                      (unsigned long)pi.kbps);
    else if (pi.count) snprintf(buf, sizeof(buf), "%d / %d", pi.index + 1, pi.count);
    else buf[0] = 0;
    set_text_if(s_pl_meta, buf);
    lv_bar_set_value(s_pl_vol_bar, g_bp.volume, LV_ANIM_OFF);
}

static void refresh_auth(void) {
    char url[96], code[16];
    bp_aliyun_get_auth(url, sizeof(url), code, sizeof(code));
    bp_bd_state_t st = bp_aliyun_state();
    if (st == BP_BD_WAIT_QR) {
        // 扫码登录:显示二维码,按登录阶段更新提示。
        lv_obj_clear_flag(s_auth_qr_box, LV_OBJ_FLAG_HIDDEN);
        char qr[512];
        int qst = 0;
        bp_aliyun_qr_get(qr, sizeof(qr), &qst);
        static char s_last_qr[512];
        if (qr[0] && strcmp(qr, s_last_qr) != 0) {
            lv_qrcode_update(s_auth_qr, qr, (uint32_t)strlen(qr));
            strlcpy(s_last_qr, qr, sizeof(s_last_qr));
        }
        set_text_if(s_auth_code, qr[0] ? "手机阿里云盘 App 扫码" : "");
        const char *hint;
        if (!g_bp.wifi_up) hint = "请先连接 Wi-Fi\n设置 → 无线网络";
        else if (qst == 2) hint = "已扫码，请在手机上确认";
        else if (qst == 3) hint = "登录已结束，请返回设置重试";
        else hint = "请用阿里云盘 App 扫描二维码";
        set_text_if(s_auth_hint, hint);
        // 注意:s_auth_hint 构建时已定位在 y=262(二维码下方第二行),不要再用
        // set_y 移动,否则会与 y=232 的 s_auth_code 重叠产生文字残影。
        return;
    }
    // 非扫码模式(串口令牌/已授权):隐藏二维码,维持原提示。
    lv_obj_add_flag(s_auth_qr_box, LV_OBJ_FLAG_HIDDEN);
    set_text_if(s_auth_code, "");
    const char *hint;
    if (st == BP_BD_READY) hint = "授权成功";
    else if (!g_bp.wifi_up) hint = "请先连接 Wi-Fi\n设置 → 无线网络";
    else if (st == BP_BD_WAIT_CODE) hint = "正在校验令牌…";
    else hint = "按 OK 开始扫码登录";   // 不再提示串口 ALI TOKEN
    set_text_if(s_auth_hint, hint);
    lv_obj_set_y(s_auth_hint, 140);
}

static void refresh_brightness(void) {
    int level = g_bp.brightness;
    for (int i = 0; i < BP_BRIGHTNESS_LEVELS; i++)
        lv_obj_set_style_bg_color(s_bri_bars[i], lv_color_hex(i < level ? COL_ACCENT : COL_SURFACE2), 0);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d / %d", level, BP_BRIGHTNESS_LEVELS);
    set_text_if(s_bri_value, buf);
}

static void refresh_settings(void) {
    char bri[12], ssid[33] = "";
    snprintf(bri, sizeof(bri), "%d / %d", g_bp.brightness, BP_BRIGHTNESS_LEVELS);
    if (g_bp.wifi_up) bp_wifi_get_ssid(ssid, sizeof(ssid));
    const char *vals[BP_MENU_COUNT] = {
        "", "", BP_SKIN_NAMES[g_bp.skin], bri,
        g_bp.wifi_up ? ssid : "未连接", bp_aliyun_state() == BP_BD_READY ? "已绑定" : "未绑定", "", "",
    };
    int sel = s_sel[BP_PAGE_SETTINGS];
    int first = sel - MENU_ROWS / 2;
    if (first > BP_MENU_COUNT - MENU_ROWS) first = BP_MENU_COUNT - MENU_ROWS;
    if (first < 0) first = 0;
    for (int i = 0; i < MENU_ROWS; i++) {
        int item = first + i;
        bool on = item == sel;
        set_text_if(s_set_icons[i], MENU_ICONS[item]);
        set_text_if(s_set_texts[i], MENU_TEXTS[item]);
        set_text_if(s_set_vals[i], vals[item]);
        pill_select(s_set_items[i], s_set_icons[i], s_set_texts[i], on);
        lv_obj_set_style_text_color(s_set_vals[i], lv_color_hex(on ? COL_ON_ACC : COL_MUTED), 0);
    }
    lv_obj_set_y(s_set_thumb, 222 * first / BP_MENU_COUNT);
}

static void refresh_skin(void) {
    int sel = s_sel[BP_PAGE_SKIN];
    for (int i = 0; i < BP_SKIN_COUNT; i++) {
        bool used = (int)g_bp.skin == i;
        lv_obj_set_style_border_width(s_skin_card[i], i == sel ? 3 : 1, 0);
        lv_obj_set_style_border_color(s_skin_card[i], lv_color_hex(i == sel ? 0xFFFFFF : 0x2C3A55), 0);
        lv_obj_set_style_text_color(s_skin_name[i], lv_color_hex(used ? COL_ACCENT : COL_TEXT), 0);
        if (used) lv_obj_remove_flag(s_skin_tag[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_skin_tag[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// 磁带界面自带状态栏;显示它时隐藏全局状态栏,避免重叠。
static void apply_status_visibility(void) {
    bool hide = s_page == BP_PAGE_PLAYER && bp_skin_is_tape(g_bp.skin);
    lv_obj_t *items[] = {s_st_wifi, s_st_play, s_st_batt};
    for (size_t i = 0; i < 3; i++) {
        if (hide) lv_obj_add_flag(items[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(items[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_player_page(void) {
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    // 开机续播提示:曲目开始(或续播失败)前,用曲名/目录两行告诉用户正在做什么。
    if (s_hint_on && (pi.name[0] || pi.state == BP_PLAY_ERROR)) {
        s_hint_on = false;
        ESP_LOGI("bp_ui", "resume hint: done (%s)", pi.name[0] ? "playing" : "error");
    }
    if (s_hint_on) {
        strlcpy(pi.name, s_hint_line, sizeof(pi.name));
        strlcpy(pi.folder, s_hint_folder, sizeof(pi.folder));
    }
    if (bp_skin_is_tape(g_bp.skin)) bp_reel_refresh(&pi);
    else refresh_player(&pi);
}

void bp_ui_set_resume_hint(const char *line, const char *folder) {
    ESP_LOGI("bp_ui", "resume hint: %s %s", line ? line : "(cleared)", folder ? folder : "");
    s_hint_on = line != NULL;
    strlcpy(s_hint_line, line ? line : "", sizeof(s_hint_line));
    strlcpy(s_hint_folder, folder ? folder : "", sizeof(s_hint_folder));
    if (s_page == BP_PAGE_PLAYER) refresh_player_page();
}

// 磁带界面动画:只在它显示时运行,实际间隔按时钟计算。
static void anim_timer_cb(lv_timer_t *t) {
    (void)t;
    int64_t now = esp_timer_get_time() / 1000;
    uint32_t dt = s_anim_last ? (uint32_t)(now - s_anim_last) : ANIM_MS;
    s_anim_last = now;
    if (s_page != BP_PAGE_PLAYER || !bp_skin_is_tape(g_bp.skin) || g_bp.screen_off) return;
    if (dt > 200) dt = 200;
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    bp_reel_tick(dt, &pi);
}

static void refresh_wifi(void) {
    char ssid[33] = "", ip[20] = "", buf[256];
    int saved = bp_wifi_saved_count();
    if (g_bp.wifi_up) {
        bp_wifi_get_ssid(ssid, sizeof(ssid));
        bp_wifi_get_ip(ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "已连接 %s\nIP %s\n信号 %d dBm\n已保存 %d 个网络",
                 ssid, ip, bp_wifi_get_rssi(), saved);
    } else {
        snprintf(buf, sizeof(buf), "%s\n已保存 %d 个网络\n\n按 OK 开启热点，用手机网页配网",
                 saved ? "正在连接…" : "未配置网络", saved);
    }
    set_text_if(s_wifi_info, buf);
}

static void refresh_wifi_ap(void) {
    char ssid[33], payload[64];
    bp_wifi_config_ssid(ssid, sizeof(ssid));
    if (ssid[0] && strcmp(ssid, s_ap_shown)) {
        // 标准 Wi-Fi 二维码:手机相机扫描即可加入开放热点。
        int n = snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
        lv_qrcode_update(s_ap_qr, payload, (uint32_t)n);
        strlcpy(s_ap_shown, ssid, sizeof(s_ap_shown));
    }
    set_text_if(s_ap_ssid, ssid);
}

static void refresh_account(void) {
    bp_bd_state_t st = bp_aliyun_state();
    set_text_if(s_acct_state, st == BP_BD_READY ? "状态：已授权\n可以浏览和播放网盘音频" :
                              "状态：未授权\n需要绑定阿里云盘");
    set_text_if(s_acct_action, st == BP_BD_READY ? (s_acct_armed ? "确认退出" : "退出登录") : "绑定账号");
    set_text_if(s_acct_hint, st == BP_BD_READY ?
                (s_acct_armed ? "再按一次 OK 退出\n网盘文件不受影响" : "退出只清除本机授权") :
                "按 OK 查看绑定说明");
}

static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    refresh_status();
    refresh_toast();
    // 网页配网完成(完成页请求 /exit 后热点关闭)后回到网络页看连接结果。
    if (s_page == BP_PAGE_WIFI_AP && !bp_wifi_config_active()) {
        bp_ui_goto(BP_PAGE_WIFI);
        bp_ui_toast("配网完成，正在连接");
    }
    switch (s_page) {
        case BP_PAGE_LIST: refresh_list(); break;
        case BP_PAGE_PLAYER: refresh_player_page(); break;
        case BP_PAGE_AUTH: refresh_auth(); break;
        case BP_PAGE_SETTINGS: refresh_settings(); break;
        case BP_PAGE_WIFI: refresh_wifi(); break;
        case BP_PAGE_ACCOUNT: refresh_account(); break;
        case BP_PAGE_WIFI_AP: refresh_wifi_ap(); break;
        case BP_PAGE_BRIGHTNESS: refresh_brightness(); break;
        case BP_PAGE_SKIN: refresh_skin(); break;
        default: break;
    }
    // 整次 UI 刷新完整跑完才打点:心跳据此判断 taskLVGL 是否假死(如堆耗尽触发的
    // LVGL assert while(1))。卡死时该时间戳停止前进,心跳会强制重启自恢复。
    s_last_refresh_us = esp_timer_get_time();
}

int64_t bp_ui_ms_since_refresh(void) {
    return (esp_timer_get_time() - s_last_refresh_us) / 1000;
}

// ---- 公共接口 -------------------------------------------------------------------
static void log_heap(const char *what) {
    ESP_LOGI("bp_ui", "heap after %s: free=%u largest=%u", what, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void bp_ui_init(void) {
    log_heap("start");
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_list();
    build_player();
    build_settings();
    build_top_layer();
    lv_obj_remove_flag(s_pages[BP_PAGE_PLAYER], LV_OBJ_FLAG_HIDDEN);
    bp_ui_apply_skin();
    ui_timer_cb(NULL);
    lv_timer_create(ui_timer_cb, 200, NULL);
    lv_timer_create(anim_timer_cb, ANIM_MS, NULL);
    log_heap("init");
}

void bp_ui_goto(bp_page_t page) {
    if (page >= BP_PAGE_COUNT) return;
    if (page != s_page) {
        if (!s_pages[page] && LAZY_BUILDERS[page]) LAZY_BUILDERS[page]();
        if (!s_pages[page]) return;
        lv_obj_remove_flag(s_pages[page], LV_OBJ_FLAG_HIDDEN);
        if (LAZY_BUILDERS[s_page]) {   // 不常用页面:离开即删除,释放内存
            lv_obj_delete(s_pages[s_page]);
            s_pages[s_page] = NULL;
        } else {
            lv_obj_add_flag(s_pages[s_page], LV_OBJ_FLAG_HIDDEN);
        }
        s_page = page;
        apply_status_visibility();
    }
    if (page == BP_PAGE_ACCOUNT) s_acct_armed = false;
    if (page == BP_PAGE_AUTH) s_auth_shown[0] = 0;
    if (page == BP_PAGE_WIFI_AP) s_ap_shown[0] = 0;
    switch (page) {   // 立即刷新,不等定时器
        case BP_PAGE_LIST: refresh_list(); break;
        case BP_PAGE_PLAYER: refresh_player_page(); break;
        case BP_PAGE_AUTH: refresh_auth(); break;
        case BP_PAGE_SETTINGS: refresh_settings(); break;
        case BP_PAGE_WIFI: refresh_wifi(); break;
        case BP_PAGE_ACCOUNT: refresh_account(); break;
        case BP_PAGE_WIFI_AP: refresh_wifi_ap(); break;
        case BP_PAGE_BRIGHTNESS: refresh_brightness(); break;
        case BP_PAGE_SKIN: refresh_skin(); break;
        default: break;
    }
}

bp_page_t bp_ui_page(void) { return s_page; }

void bp_ui_toast(const char *text) {
    portENTER_CRITICAL(&s_toast_mux);
    strlcpy(s_toast_text, text ? text : "", sizeof(s_toast_text));
    s_toast_pending = true;
    portEXIT_CRITICAL(&s_toast_mux);
}

void bp_ui_move(int delta) {
    int count = s_page == BP_PAGE_SETTINGS ? BP_MENU_COUNT :
                s_page == BP_PAGE_SKIN ? BP_SKIN_COUNT :
                s_page == BP_PAGE_LIST ? s_list_rows : 0;
    if (count <= 0) return;
    int sel = s_sel[s_page] + delta;
    // 单步循环;大步(长按)夹到两端。
    if (delta == 1 || delta == -1) sel = bp_playlist_move(s_sel[s_page], count, delta, true);
    else if (sel < 0) sel = 0;
    else if (sel >= count) sel = count - 1;
    s_sel[s_page] = sel;
    bp_ui_goto(s_page);
}

int bp_ui_selected(void) { return s_sel[s_page]; }

void bp_ui_set_selected(bp_page_t page, int sel) {
    if (page < BP_PAGE_COUNT) s_sel[page] = sel;
}

void bp_ui_apply_skin(void) {
    bool reel = bp_skin_is_tape(g_bp.skin);
    if (s_built_skin != (int)g_bp.skin) {   // 只保留当前皮肤的对象
        lv_obj_clean(s_pl_reel);
        lv_obj_clean(s_pl_classic);
        if (reel) bp_reel_build(s_pl_reel, (bp_skin_t)g_bp.skin);
        else build_classic(s_pl_classic);
        s_built_skin = g_bp.skin;
    }
    if (reel) {
        lv_obj_remove_flag(s_pl_reel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pl_classic, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_pl_classic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pl_reel, LV_OBJ_FLAG_HIDDEN);
    }
    apply_status_visibility();
    if (s_page == BP_PAGE_PLAYER) refresh_player_page();
}

void bp_ui_reel_kick(int dir) { bp_reel_kick(dir); }

void bp_ui_volume_feedback(void) {
    if (bp_skin_is_tape(g_bp.skin)) bp_reel_volume();
    if (s_page == BP_PAGE_PLAYER) refresh_player_page();
}

bp_row_kind_t bp_ui_list_row(int *file_index) {
    if (s_list.status != 2 && s_list.status != 1) return BP_ROW_RETRY;
    if (s_list.status == 1) return BP_ROW_NONE;
    if (s_list_rows == 0) return BP_ROW_RETRY;
    return row_kind(s_sel[BP_PAGE_LIST], file_index);
}

void bp_ui_list_reset_sel(void) { s_sel[BP_PAGE_LIST] = 0; }

void bp_ui_set_onboarding(bool on) {
    s_onboarding = on;
    if (s_pages[BP_PAGE_WIFI_AP]) set_text_if(s_ap_title, on ? "第1步 连接网络" : "网页配网");
    if (s_pages[BP_PAGE_AUTH]) set_text_if(s_auth_title, on ? "第2步 绑定网盘" : "连接阿里云盘");
}

void bp_ui_account_arm(bool armed) {
    s_acct_armed = armed;
    if (s_page == BP_PAGE_ACCOUNT) refresh_account();
}
