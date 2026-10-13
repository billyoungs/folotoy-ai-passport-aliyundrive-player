// main/bp_ui_reel.c —— 磁带风格播放界面(磁带为品牌绿底;人物磁带换盘面与底色)。
//
// 布局(240x320):顶部状态栏与进度线;左右两个开盘磁带轮水平对齐,中间是 TRACK
// 与曲目号;走带线上一黑一白两条声波;下方时间、歌名、文件夹、滚动信息行;
// 底部一条切成两段的鱼(参考 docs/fish.mov),鱼头(左)与鱼尾(右)之间是一排声波竖条。
//
// 性能(单核 160 MHz,同时在下载与解码):
// - 盘面三重对称:0..120° 每 5° 预渲染一帧存在 flash(tools/generate_reel_disc.py),
//   转动时只切换帧,不做运行时旋转(不占临时缓冲、不耗 CPU 变换);
//   磁带卷是盘面后面的圆,透过窗口露出,只在卷径变化时调整。
// - 人物盘面(tools/generate_character_discs.py)没有对称性,整圈每 10° 一帧。
// - 两条走带声波用 lv_line,只更新点坐标,每条线限定在一个窄条区域内重绘。
// - 鱼头、鱼尾是 flash 中的 A8 线稿(tools/generate_fish.py),按摆动幅度预渲染 9 帧,
//   播放时像参考视频那样缓慢摆动(切帧加 1~2 像素浮动);声波竖条在一个对象的
//   绘制回调里画出,不额外创建 LVGL 对象。
// - 转轮停稳且声波收平后不再更新,暂停时不占 CPU。
#include "bp_ui_reel.h"

#include "bsp_battery.h"
#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(bp_font_16);
extern const lv_image_dsc_t bp_reel_disc[24];
extern const lv_image_dsc_t bp_disc_bluey[36], bp_disc_bingo[36], bp_disc_peppa[36];
#define FISH_FRAMES 9
extern const lv_image_dsc_t bp_fish_head[FISH_FRAMES], bp_fish_tail[FISH_FRAMES];

#define COL_INK    0x2A0A33   // 紫色主题下的墨色(原绿 0x092113)
#define COL_TAPE_R 0x7B1FA2
#define COL_WHITE  0xFFFFFF
#define COL_ALERT  0x8C1D18

#define CX_L 62
#define CX_R 178
#define CY 74
#define DISC_R 38
#define TAPE_Y 141           // 走带线基线
#define WAVE_BAND 12         // 声波上下各留的绘制高度
#define FISH_Y 280
#define FISH_PERIOD 3.6f     // 鱼摆动周期(秒)
#define TICKER_H 22
#define SPIN_K 3900.0f       // 角速度 = 线速度系数 / 卷径(度/秒)

#define N_DARK 47            // x 28..212 步长 4
#define N_WHITE 43           // x 36..204
#define N_BARS 15            // 鱼身声波竖条
#define BAR_X 70             // 竖条区域 x 70..160
#define BAR_STEP 6
#define BAR_HALF 11          // 竖条最大半高
#define HEAD_X 14            // 鱼头/鱼尾图片位置(图片四周各留 3 像素摆动余量)
#define TAIL_X 165

// 磁带界面的变体:盘面帧(左/右)、帧数、一个周期的角度、底色、是否画中心轴。
typedef struct {
    const lv_image_dsc_t *disc[2];
    int frames;
    float period;
    uint32_t bg;
    bool hub;
} reel_variant_t;

static const reel_variant_t VARIANTS[] = {
    // 盘面三重对称:0..120° 共 24 帧。底色为主题紫色(原绿色 0x20E47C)。
    {{bp_reel_disc, bp_reel_disc}, 24, 120.0f, 0x9C27B0, true},
    // 人物盘面没有对称性:整圈 36 帧;脸在中心,不画中心轴。
    {{bp_disc_bluey, bp_disc_bingo}, 36, 360.0f, 0x5CB8EC, false},
    {{bp_disc_peppa, bp_disc_peppa}, 36, 360.0f, 0xF58FB3, false},
};
static const reel_variant_t *s_var = &VARIANTS[0];

static lv_obj_t *s_wifi, *s_clock, *s_right, *s_prog_fill, *s_prog_dot;
static lv_obj_t *s_tape[2], *s_disc[2], *s_slant[2];
static lv_point_precise_t s_slant_pts[2][2];
static lv_obj_t *s_dark, *s_white, *s_bars;
static lv_point_precise_t s_dark_pts[N_DARK], s_white_pts[N_WHITE];
static float s_bar_h[N_BARS], s_bar_t[N_BARS];   // 竖条当前/目标半高(像素)
static uint32_t s_bar_seed = 0x2545F491, s_bar_ms;
static lv_obj_t *s_fish_head, *s_fish_tail;
static float s_fish_ph;
static int s_fish_hf = -1, s_fish_tf = -1, s_fish_hy, s_fish_ty;
static lv_obj_t *s_track_lbl, *s_track_no, *s_time, *s_title, *s_folder;
static lv_obj_t *s_ticker_in, *s_ticker[4];
static int s_ticker_idx = -1;
static lv_obj_t *s_vol_box, *s_vol_seg[10];
static int64_t s_vol_until;

static float s_angle[2], s_spin, s_phase, s_tape_r[2], s_level;
static int s_kick;
static int64_t s_kick_until;
static bool s_settled;
static int s_frame[2];

// ---- 小工具 ---------------------------------------------------------------------
static lv_obj_t *rect(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// 圆:fill 为 -1 表示只描边。
static lv_obj_t *circle(lv_obj_t *p, int cx, int cy, int r, int32_t fill, int border, uint32_t bc) {
    lv_obj_t *o = rect(p, cx - r, cy - r, 2 * r, 2 * r, fill < 0 ? 0 : (uint32_t)fill, LV_RADIUS_CIRCLE);
    if (fill < 0) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    if (border) {
        lv_obj_set_style_border_width(o, border, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(bc), 0);
    }
    return o;
}

static lv_obj_t *text(lv_obj_t *p, const lv_font_t *font, uint32_t color, const char *s) {
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, s);
    return l;
}

static void set_text_if(lv_obj_t *l, const char *s) {
    const char *old = lv_label_get_text(l);
    if (!old || strcmp(old, s)) lv_label_set_text(l, s);
}

// 折线对象占据 (x, y, w, h) 的窄条,点坐标相对于该区域,重绘只限于此区域。
static lv_obj_t *polyline(lv_obj_t *p, int x, int y, int w, int h, lv_point_precise_t *pts,
                          uint32_t n, uint32_t color, int width) {
    lv_obj_t *l = lv_line_create(p);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_line_set_points_mutable(l, pts, n);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

static uint32_t rnd(void) {   // xorshift32,声波竖条的随机起伏
    s_bar_seed ^= s_bar_seed << 13;
    s_bar_seed ^= s_bar_seed >> 17;
    s_bar_seed ^= s_bar_seed << 5;
    return s_bar_seed;
}

// 鱼身声波:每根竖条是上下对称的圆角条,静止时缩成小点。
static void bars_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(s_bars, &c);
    int32_t mid = (c.y1 + c.y2) / 2;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(COL_WHITE);
    d.radius = 1;
    for (int i = 0; i < N_BARS; i++) {
        int32_t h = (int32_t)(s_bar_h[i] + 0.5f);
        if (h < 1) h = 1;
        lv_area_t a = {c.x1 + 2 + i * BAR_STEP, mid - h, c.x1 + 3 + i * BAR_STEP, mid + h};
        lv_draw_rect(layer, &d, &a);
    }
}

static void fmt_time(uint32_t ms, char *out, size_t cap) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(out, cap, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                            (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(out, cap, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// 鱼的摆动:按相位选帧并上下浮动;鱼头与鱼尾错开相位,只在帧或位置变化时更新。
static void fish_pose(void) {
    float sh = sinf(s_fish_ph + 2.0f), st = sinf(s_fish_ph);
    int hf = (int)lroundf((sh + 1) * 0.5f * (FISH_FRAMES - 1));
    int tf = (int)lroundf((st + 1) * 0.5f * (FISH_FRAMES - 1));
    int hy = (int)lroundf(1.5f * sinf(s_fish_ph + 3.2f)), ty = (int)lroundf(1.2f * st);
    if (hf != s_fish_hf) lv_image_set_src(s_fish_head, &bp_fish_head[s_fish_hf = hf]);
    if (tf != s_fish_tf) lv_image_set_src(s_fish_tail, &bp_fish_tail[s_fish_tf = tf]);
    if (hy != s_fish_hy) lv_obj_set_pos(s_fish_head, HEAD_X, FISH_Y - 21 + (s_fish_hy = hy));
    if (ty != s_fish_ty) lv_obj_set_pos(s_fish_tail, TAIL_X, FISH_Y - 19 + (s_fish_ty = ty));
}

// ---- 构建 -------------------------------------------------------------------------
void bp_reel_build(lv_obj_t *parent, bp_skin_t skin) {
    s_var = &VARIANTS[skin == BP_SKIN_BLUEY ? 1 : skin == BP_SKIN_PEPPA ? 2 : 0];
    s_ticker_idx = -1;
    s_vol_until = 0;
    s_settled = false;
    s_kick = 0;
    s_frame[0] = s_frame[1] = 0;
    lv_obj_t *p = rect(parent, 0, 0, 240, 320, s_var->bg, 0);

    // 状态栏:左 Wi-Fi + 时间,右 音量 + 电量;下方细进度线。
    s_wifi = text(p, &lv_font_montserrat_14, COL_INK, LV_SYMBOL_WIFI);
    lv_obj_set_pos(s_wifi, 30, 7);
    s_clock = text(p, &lv_font_montserrat_14, COL_INK, "--:--");
    lv_obj_set_pos(s_clock, 50, 7);
    s_right = text(p, &lv_font_montserrat_14, COL_INK, "");
    lv_obj_align(s_right, LV_ALIGN_TOP_RIGHT, -30, 7);
    lv_obj_t *track = rect(p, 24, 27, 192, 2, COL_INK, 1);
    lv_obj_set_style_bg_opa(track, LV_OPA_30, 0);
    s_prog_fill = rect(p, 24, 27, 1, 2, COL_INK, 1);
    s_prog_dot = circle(p, 24, 28, 3, COL_WHITE, 1, COL_INK);

    // 磁带卷(盘面后面)与引向导轮的斜线。
    s_tape[0] = circle(p, CX_L, CY, 36, COL_INK, 0, 0);
    s_tape[1] = circle(p, CX_R, CY, 15, COL_TAPE_R, 0, 0);
    for (int i = 0; i < 2; i++) {
        s_slant_pts[i][0] = (lv_point_precise_t){i ? CX_R + 15 : CX_L - 36, CY + 2 - 30};
        s_slant_pts[i][1] = (lv_point_precise_t){i ? 218 : 22, 134 - 30};
        s_slant[i] = polyline(p, 0, 30, 240, 110, s_slant_pts[i], 2, COL_INK, 2);
    }

    // 盘面:预渲染的旋转帧,按角度切换。
    for (int i = 0; i < 2; i++) {
        int cx = i ? CX_R : CX_L;
        s_disc[i] = lv_image_create(p);
        lv_image_set_src(s_disc[i], &s_var->disc[i][0]);
        lv_obj_set_pos(s_disc[i], cx - DISC_R, CY - DISC_R);
        if (!s_var->hub) continue;
        circle(p, cx, CY, 9, COL_WHITE, 2, COL_INK);
        circle(p, cx, CY, 3, i ? COL_INK : COL_TAPE_R, 0, 0);
    }

    // 两轮之间:TRACK + 曲目号 + 三道装饰线。
    s_track_lbl = text(p, &lv_font_montserrat_10, COL_INK, "TRACK");
    lv_obj_set_width(s_track_lbl, 40);
    lv_obj_set_style_text_align(s_track_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_track_lbl, 100, 48);
    s_track_no = text(p, &lv_font_montserrat_28, COL_INK, "--");
    lv_obj_set_width(s_track_no, 40);
    lv_obj_set_style_text_align(s_track_no, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_track_no, 100, 60);
    rect(p, 106, 96, 28, 1, COL_INK, 0);
    rect(p, 106, 99, 28, 1, COL_INK, 0);
    rect(p, 110, 102, 20, 1, COL_INK, 0);

    // 走带线:黑、白两条声波 + 两个导轮。
    for (int i = 0; i < N_DARK; i++) s_dark_pts[i] = (lv_point_precise_t){28 + i * 4, WAVE_BAND};
    for (int i = 0; i < N_WHITE; i++) s_white_pts[i] = (lv_point_precise_t){36 + i * 4, WAVE_BAND};
    s_dark = polyline(p, 0, TAPE_Y - WAVE_BAND, 240, 2 * WAVE_BAND, s_dark_pts, N_DARK, COL_INK, 2);
    s_white = polyline(p, 0, TAPE_Y - WAVE_BAND, 240, 2 * WAVE_BAND, s_white_pts, N_WHITE, COL_WHITE, 2);
    circle(p, 28, 134, 6, COL_WHITE, 2, COL_INK);
    circle(p, 212, 134, 6, COL_WHITE, 2, COL_INK);

    s_time = text(p, &lv_font_montserrat_14, COL_INK, "00:00 / --:--");
    lv_obj_set_width(s_time, 240);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_time, 0, 152);

    // 歌名、文件夹(各带一个小图标)。
    lv_obj_t *ic = text(p, &lv_font_montserrat_14, COL_INK, LV_SYMBOL_AUDIO);
    lv_obj_set_pos(ic, 26, 181);
    s_title = text(p, &bp_font_16, COL_INK, "");
    lv_obj_set_width(s_title, 172);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_title, 44, 178);
    ic = text(p, &lv_font_montserrat_14, COL_INK, LV_SYMBOL_DIRECTORY);
    lv_obj_set_pos(ic, 26, 203);
    s_folder = text(p, &bp_font_16, COL_INK, "");
    lv_obj_set_size(s_folder, 172, lv_font_get_line_height(lv_obj_get_style_text_font(s_folder, 0)));
    lv_label_set_long_mode(s_folder, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_folder, 44, 200);

    // 滚动信息行:4 行叠放,定期向上滑动一行。
    lv_obj_t *tk = rect(p, 24, 228, 192, TICKER_H, s_var->bg, 0);
    lv_obj_set_style_bg_opa(tk, LV_OPA_TRANSP, 0);
    s_ticker_in = rect(tk, 0, 0, 192, TICKER_H * 4, s_var->bg, 0);
    lv_obj_set_style_bg_opa(s_ticker_in, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 4; i++) {
        s_ticker[i] = text(s_ticker_in, &bp_font_16, COL_INK, "");
        lv_obj_set_size(s_ticker[i], 192, lv_font_get_line_height(lv_obj_get_style_text_font(s_ticker[i], 0)));
        lv_label_set_long_mode(s_ticker[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(s_ticker[i], 0, i * TICKER_H);
    }

    // 鱼:切成两段,鱼头朝左、鱼尾朝右,中间的鱼身是声波竖条。
    s_fish_head = lv_image_create(p);
    s_fish_tail = lv_image_create(p);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *o = i ? s_fish_tail : s_fish_head;
        lv_obj_set_style_image_recolor(o, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
    }
    s_fish_hf = s_fish_tf = -1;
    s_fish_hy = s_fish_ty = 99;
    fish_pose();
    s_bars = rect(p, BAR_X, FISH_Y - BAR_HALF - 1, N_BARS * BAR_STEP, 2 * BAR_HALF + 3, s_var->bg, 0);
    lv_obj_set_style_bg_opa(s_bars, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_bars, bars_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    for (int i = 0; i < N_BARS; i++) s_bar_h[i] = s_bar_t[i] = 1;

    // 音量浮层。
    s_vol_box = rect(p, 40, 260, 160, 30, COL_INK, 15);
    lv_obj_t *vl = text(s_vol_box, &bp_font_16, COL_WHITE, "音量");
    lv_obj_align(vl, LV_ALIGN_LEFT_MID, 16, 0);
    for (int i = 0; i < 10; i++) s_vol_seg[i] = rect(s_vol_box, 60 + i * 9, 10, 6, 10, COL_WHITE, 1);
    lv_obj_add_flag(s_vol_box, LV_OBJ_FLAG_HIDDEN);

    s_tape_r[0] = 36;
    s_tape_r[1] = 15;
}

// ---- 刷新(文字) -------------------------------------------------------------------
static void set_tape(int i, float r) {
    if (fabsf(r - s_tape_r[i]) < 0.5f) return;
    s_tape_r[i] = r;
    int ri = (int)(r + 0.5f), cx = i ? CX_R : CX_L;
    lv_obj_set_pos(s_tape[i], cx - ri, CY - ri);
    lv_obj_set_size(s_tape[i], 2 * ri, 2 * ri);
    s_slant_pts[i][0].x = i ? CX_R + r : CX_L - r;
    lv_obj_invalidate(s_slant[i]);
}

void bp_reel_refresh(const bp_player_info_t *pi) {
    char buf[64], a[16], b[16];
    int64_t now = esp_timer_get_time() / 1000;
    bool idle = pi->state == BP_PLAY_IDLE && !pi->name[0];

    lv_obj_set_style_text_color(s_wifi, lv_color_hex(g_bp.wifi_up ? COL_INK : COL_ALERT), 0);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 >= 2024) snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    else strlcpy(buf, "--:--", sizeof(buf));
    set_text_if(s_clock, buf);
    // 电量只显示图标(按电量分档),不显示数字。
    int soc = bsp_battery_soc();
    const char *batt = soc < 0 ? "" : soc > 80 ? LV_SYMBOL_BATTERY_FULL : soc > 55 ? LV_SYMBOL_BATTERY_3
                     : soc > 30 ? LV_SYMBOL_BATTERY_2 : soc > 10 ? LV_SYMBOL_BATTERY_1
                     : LV_SYMBOL_BATTERY_EMPTY;
    snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX " %u   %s", g_bp.volume, batt);
    set_text_if(s_right, buf);

    uint32_t prog = pi->total_ms ? (uint32_t)((uint64_t)pi->pos_ms * 192 / pi->total_ms) : 0;
    if (prog > 192) prog = 192;
    lv_obj_set_width(s_prog_fill, prog ? (int32_t)prog : 1);
    lv_obj_set_x(s_prog_dot, 24 + (int32_t)prog - 3);

    // TRACK 区:暂停时 PAUSE 闪烁,快进/倒带时显示 FFWD / REW。
    const char *lbl = "TRACK";
    if (s_kick > 0) lbl = "FFWD";
    else if (s_kick < 0) lbl = "REW";
    else if (pi->state == BP_PLAY_PAUSED) lbl = (now / 500) % 2 ? "" : "PAUSE";
    set_text_if(s_track_lbl, lbl);
    if (idle) strlcpy(buf, "--", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%02d", pi->index + 1);
    set_text_if(s_track_no, buf);

    fmt_time(pi->pos_ms, a, sizeof(a));
    if (pi->total_ms) fmt_time(pi->total_ms, b, sizeof(b));
    else strlcpy(b, "--:--", sizeof(b));
    snprintf(buf, sizeof(buf), "%s / %s", a, b);
    set_text_if(s_time, buf);

    set_text_if(s_title, pi->name[0] ? pi->name : "未在播放");
    set_text_if(s_folder, pi->folder[0] ? pi->folder : idle ? "阿里云盘随身听" : "");

    // 信息行:格式码率 · 第几首 · 文件夹 · 时长;出错或空闲时换成提示。
    const char *lines[4];
    char l0[32], l1[32], l2[48], l3[32];
    int n = 0;
    if (pi->state == BP_PLAY_ERROR) {
        lines[n++] = pi->error ? pi->error : "播放失败";
    } else if (idle) {
        lines[n++] = "短按 OK 选择音频";
        lines[n++] = "长按 OK 打开设置";
    } else if (pi->state == BP_PLAY_RESOLVING || pi->state == BP_PLAY_BUFFERING) {
        snprintf(l0, sizeof(l0), "正在缓冲 %u%%", pi->buffer_pct);
        lines[n++] = l0;
    } else {
        if (pi->fmt && pi->kbps) { snprintf(l0, sizeof(l0), "%s · %lu kbps", pi->fmt, (unsigned long)pi->kbps); lines[n++] = l0; }
        if (pi->count) { snprintf(l1, sizeof(l1), "第 %d / %d 首", pi->index + 1, pi->count); lines[n++] = l1; }
        if (pi->folder[0]) { snprintf(l2, sizeof(l2), "文件夹 · %s", pi->folder); lines[n++] = l2; }
        if (pi->total_ms) { fmt_time(pi->total_ms, b, sizeof(b)); snprintf(l3, sizeof(l3), "时长 · %s", b); lines[n++] = l3; }
        if (!n) lines[n++] = "阿里云盘随身听";
    }
    for (int i = 0; i < 4; i++) set_text_if(s_ticker[i], i < n ? lines[i] : "");
    int idx = n > 1 ? (int)((now / 3000) % n) : 0;
    if (idx != s_ticker_idx) {
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, s_ticker_in);
        lv_anim_set_exec_cb(&an, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&an, lv_obj_get_y(s_ticker_in), -idx * TICKER_H);
        lv_anim_set_duration(&an, s_ticker_idx < 0 ? 0 : 450);
        lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
        lv_anim_start(&an);
        s_ticker_idx = idx;
    }

    if (s_vol_until) {
        for (int i = 0; i < 10; i++)
            lv_obj_set_style_bg_opa(s_vol_seg[i], i < (g_bp.volume + 5) / 10 ? LV_OPA_COVER : LV_OPA_30, 0);
        if (now > s_vol_until) {
            lv_obj_add_flag(s_vol_box, LV_OBJ_FLAG_HIDDEN);
            s_vol_until = 0;
        }
    }
}

// ---- 动画 -------------------------------------------------------------------------
void bp_reel_tick(uint32_t dt_ms, const bp_player_info_t *pi) {
    float dt = dt_ms / 1000.0f;
    int64_t now = esp_timer_get_time() / 1000;
    if (s_kick && now > s_kick_until) s_kick = 0;

    float target = s_kick ? 7.0f * (float)s_kick : (pi->state == BP_PLAY_PLAYING ? 1.0f : 0.0f);
    float k = (s_kick ? 0.35f : 0.12f) * (float)dt_ms / 50.0f;
    if (k > 1) k = 1;
    s_spin += (target - s_spin) * k;
    s_level += ((float)pi->level / 100.0f - s_level) * 0.35f;

    float frac = pi->total_ms ? (float)pi->pos_ms / (float)pi->total_ms : 0.0f;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    set_tape(0, 15 + 21 * sqrtf(1 - frac));
    set_tape(1, 15 + 21 * sqrtf(frac));

    float aspin = fabsf(s_spin);
    float amp = 4.6f * (aspin < 1 ? aspin : 1) * (aspin > 2 ? 0.35f : 1.0f) * (0.35f + 0.65f * s_level);
    if (aspin < 0.01f && amp < 0.05f) {
        if (s_settled) return;   // 已静止:不再重绘
        s_settled = true;
        amp = 0;
    } else {
        s_settled = false;
    }

    s_phase += dt * s_spin;
    for (int i = 0; i < 2; i++) {
        s_angle[i] = fmodf(s_angle[i] + dt * s_spin * SPIN_K / s_tape_r[i], 360.0f);
        if (s_angle[i] < 0) s_angle[i] += 360.0f;
        int frame = (int)(fmodf(s_angle[i], s_var->period) * s_var->frames / s_var->period) % s_var->frames;
        if (frame != s_frame[i]) {
            s_frame[i] = frame;
            lv_image_set_src(s_disc[i], &s_var->disc[i][frame]);
        }
    }

    float t = s_phase;
    for (int i = 0; i < N_DARK; i++) {
        float x = 28.0f + i * 4;
        float env = sinf((float)M_PI * (x - 28) / 184);
        s_dark_pts[i].y = WAVE_BAND - amp * 0.55f * env *
            (0.6f * sinf(x * 0.13f + t * 6.1f) + 0.4f * sinf(x * 0.37f - t * 7.7f));
    }
    for (int i = 0; i < N_WHITE; i++) {
        float x = 36.0f + i * 4;
        float env = sinf((float)M_PI * (x - 28) / 184);
        s_white_pts[i].y = WAVE_BAND - amp * env *
            (0.65f * sinf(x * 0.21f - t * 9) + 0.35f * sinf(x * 0.53f + t * 5.3f));
    }
    // 鱼:播放时缓慢摆动,暂停时随转轮一起减速停下。
    s_fish_ph += dt * (aspin < 1 ? aspin : 1) * 2 * (float)M_PI / FISH_PERIOD;
    if (s_fish_ph > 2 * (float)M_PI) s_fish_ph -= 2 * (float)M_PI;
    fish_pose();

    // 鱼身竖条:约每 90 ms 随机换一次目标高度(随音量),快升慢落。
    s_bar_ms += dt_ms;
    float lvl = amp / 4.6f;
    if (s_bar_ms >= 90) {
        s_bar_ms = 0;
        for (int i = 0; i < N_BARS; i++) {
            float r = (float)(rnd() % 1000) / 1000.0f;
            s_bar_t[i] = 1 + (BAR_HALF - 1) * lvl * (0.2f + 0.8f * r * r);
        }
    }
    for (int i = 0; i < N_BARS; i++) {
        float kb = (s_bar_t[i] > s_bar_h[i] ? 0.6f : 0.25f) * (float)dt_ms / 50.0f;
        if (kb > 1) kb = 1;
        s_bar_h[i] += (s_bar_t[i] - s_bar_h[i]) * kb;
        if (amp == 0) s_bar_h[i] = s_bar_t[i] = 1;   // 静止:收成小点
    }
    lv_obj_invalidate(s_dark);
    lv_obj_invalidate(s_white);
    lv_obj_invalidate(s_bars);
}

void bp_reel_kick(int dir) {
    s_kick = dir > 0 ? 1 : -1;
    s_kick_until = esp_timer_get_time() / 1000 + 700;
    s_settled = false;
}

void bp_reel_volume(void) {
    lv_obj_remove_flag(s_vol_box, LV_OBJ_FLAG_HIDDEN);
    s_vol_until = esp_timer_get_time() / 1000 + 1400;
}
