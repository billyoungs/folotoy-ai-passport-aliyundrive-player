// main/bp_app.h —— 阿里云盘随身听(Aliyunpan Pocket Player)应用层公共定义。
//
// 模块划分:
//   main.c        初始化、按键分发、页面状态机、心跳
//   bp_ui.c       LVGL 界面(全部页面,只在持有 LVGL 锁时调用)
//   bp_aliyun.c   阿里云盘:refresh_token 绑定、令牌刷新、目录/音频列表、下载直链
//   bp_player.c   流式播放:HTTP 下载任务 → 环形缓冲 → MP3/WAV 解码 → I2S
//   bp_media.c    纯逻辑(主机单测)
//   bp_console.c  USB 串口命令(配网、状态、模拟按键)
//   bp_wifi.cc    esp-wifi-connect 的 C 桥接
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bp_media.h"

// 版本号规则:x.y.z 每位 0-9,某一位达到 10 则向高位进一位并归零。
// 例:0.7.9 → 0.8.0 → ... → 0.9.9 → 1.0.0。每次发布固件时按改动规模递增。
#define BP_APP_VERSION "0.11.8"
#define BP_NAME_MAX 96
// 每页条目数:8 条时部分目录的列表响应达 8191B,超过 7KB 响应缓冲导致 JSON 截断、
// 解析失败(实测 list parse failed)。改为 6 条后响应约 5-6KB,7KB 缓冲有余量。
#define BP_PAGE_SIZE 6
#define BP_PLAYLIST_MAX BP_PAGE_SIZE
#define BP_FOLDER_MAX 32
#define BP_FILE_ID_MAX 80      // 阿里云盘 file_id(字符串)
#define BP_MARKER_MAX 512     // 阿里云盘分页游标 marker(实测可超 128B,128 时截断导致翻页 400 InvalidParameter.Marker)

typedef struct {
    char file_id[BP_FILE_ID_MAX];
    uint64_t size;
    bool is_dir;
    uint8_t format;          // bp_format_t,按完整文件名判断(name 可能被缩短)
    char name[BP_NAME_MAX];  // 显示用;超长时缩短为“前半…扩展名”
    char folder[BP_FOLDER_MAX]; // 所在文件夹名(磁带界面第二行显示)
} bp_file_t;

// 列表来源:目录浏览,或网盘内全部 MP3/WAV(按修改时间倒序)。
typedef enum {
    BP_SRC_DIR = 0,
    BP_SRC_ALL_AUDIO,
} bp_source_t;

// 游标翻页:浏览目录时会过滤掉不支持的文件,所以页码不能换算成接口偏移;
// marker 为本页在网盘接口里的分页游标,next_marker 为下一页的游标。
// 历史深度取 4:每页 marker 长达 512B,历史越深 .bss 越大。4 页(32 条)对
// "上一页"回溯足够,超出后自动回到第一页;省下的 2KB .bss 让给播放环形缓冲。
#define BP_PAGE_HISTORY 4

typedef struct {
    bp_source_t source;
    int page;              // 显示用页码(0 起)
    char marker[BP_MARKER_MAX];   // 本页在网盘接口里的游标
    char dir[BP_PATH_MAX];        // 当前目录路径(显示/面包屑用)
    char parent_file_id[BP_FILE_ID_MAX]; // 要列出的目录 file_id("root" 为根)
} bp_list_req_t;

// 列表状态:0 未加载,1 加载中,2 就绪,-1 出错。
typedef struct {
    bp_list_req_t req;
    int status;
    int count;
    bool has_more;
    char next_marker[BP_MARKER_MAX];
    bp_file_t files[BP_PAGE_SIZE];
} bp_list_t;

// 开机即进入播放界面;长按 OK 打开设置菜单(全部音频、浏览网盘等都在菜单里)。
typedef enum {
    BP_PAGE_PLAYER = 0,
    BP_PAGE_LIST,
    BP_PAGE_AUTH,
    BP_PAGE_SETTINGS,
    BP_PAGE_WIFI,
    BP_PAGE_ACCOUNT,
    BP_PAGE_ABOUT,
    BP_PAGE_WIFI_AP,      // SoftAP 网页配网
    BP_PAGE_BRIGHTNESS,   // 屏幕亮度(5 档)
    BP_PAGE_SKIN,         // 播放界面选择(经典 / 磁带)
    BP_PAGE_COUNT,
} bp_page_t;

// 播放界面皮肤。
typedef enum {
    BP_SKIN_CLASSIC = 0,
    BP_SKIN_REEL,        // 磁带(以下均为磁带界面的变体,只换盘面与底色)
    BP_SKIN_BLUEY,       // 磁带 · 蓝色与橙色小狗盘面
    BP_SKIN_PEPPA,       // 磁带 · 粉色小猪盘面
    BP_SKIN_COUNT,
} bp_skin_t;

static inline bool bp_skin_is_tape(int skin) { return skin != BP_SKIN_CLASSIC; }
extern const char *const BP_SKIN_NAMES[BP_SKIN_COUNT];

// 设置菜单各项(顺序即显示顺序)。
typedef enum {
    BP_MENU_ALL_AUDIO = 0,
    BP_MENU_BROWSE,
    BP_MENU_SKIN,
    BP_MENU_BRIGHTNESS,
    BP_MENU_WIFI,
    BP_MENU_ACCOUNT,
    BP_MENU_ABOUT,
    BP_MENU_BACK,        // 返回播放页(同长按 OK)
    BP_MENU_COUNT,
} bp_menu_t;



// 网盘授权状态。
typedef enum {
    BP_BD_NO_AUTH = 0,
    BP_BD_WAIT_QR,     // 扫码登录中:屏幕显示二维码等待手机确认
    BP_BD_WAIT_CODE,   // 串口 refresh_token 校验中
    BP_BD_READY,
} bp_bd_state_t;

typedef enum {
    BP_PLAY_IDLE = 0,
    BP_PLAY_RESOLVING,    // 获取下载直链
    BP_PLAY_BUFFERING,
    BP_PLAY_PLAYING,
    BP_PLAY_PAUSED,
    BP_PLAY_ERROR,
} bp_play_state_t;

typedef struct {
    bp_play_state_t state;
    char name[BP_NAME_MAX];
    uint32_t pos_ms;
    uint32_t total_ms;     // 0 = 未知
    int index;             // 播放列表下标
    int count;             // 播放列表长度
    uint32_t rate;
    uint16_t channels;
    uint32_t kbps;
    uint8_t buffer_pct;    // 环形缓冲填充度
    uint8_t level;         // 实时音量电平 0..100(驱动磁带界面声波幅度)
    const char *fmt;       // "MP3" / "M4A" / "AAC" / "WAV",未知为 NULL
    const char *error;     // BP_PLAY_ERROR 时的中文原因
    char folder[BP_FOLDER_MAX];
} bp_player_info_t;

typedef struct {
    volatile bool wifi_up;
    volatile bool screen_off;
    volatile uint8_t volume;       // 0..100
    volatile uint8_t brightness;   // 屏幕亮度档位 1..BP_BRIGHTNESS_LEVELS
    volatile uint8_t skin;         // bp_skin_t
} bp_state_t;

extern bp_state_t g_bp;

// ---- UI(除 bp_ui_toast 外,调用方必须持有 bsp_lvgl_lock) -----------------------
typedef enum {
    BP_ROW_NONE = 0,
    BP_ROW_FILE,      // *file_index 为列表下标
    BP_ROW_PREV,
    BP_ROW_NEXT,
    BP_ROW_RETRY,     // 列表加载失败或为空时的重试
} bp_row_kind_t;

void bp_ui_init(void);
// 距上次完整 UI 刷新的毫秒数;心跳用来检测 taskLVGL 假死(>10s 则重启自恢复)。
int64_t bp_ui_ms_since_refresh(void);
void bp_ui_goto(bp_page_t page);
bp_page_t bp_ui_page(void);
void bp_ui_toast(const char *text);             // 任意任务可调用(内部只拷贝)
void bp_ui_move(int delta);                     // 当前页选中项移动
int  bp_ui_selected(void);                      // 设置菜单 / 皮肤选择页的选中项
void bp_ui_set_selected(bp_page_t page, int sel);
void bp_ui_apply_skin(void);                    // g_bp.skin 变化后切换播放界面
void bp_ui_set_resume_hint(const char *line, const char *folder);  // 开机续播提示;NULL 清除
void bp_ui_reel_kick(int dir);                  // 磁带界面:长按切歌时转轮快进(+1)/倒带(-1)
void bp_ui_volume_feedback(void);               // 音量变化的界面反馈
bp_row_kind_t bp_ui_list_row(int *file_index);  // 列表页选中行
void bp_ui_list_reset_sel(void);
void bp_ui_account_arm(bool armed);             // 账号页“再按一次确认退出”
void bp_ui_set_onboarding(bool on);             // 设置向导中:配网/绑定页显示“第N步”标题
// ---- 阿里云盘 ----------------------------------------------------------------
void bp_aliyun_init(void);
bp_bd_state_t bp_aliyun_state(void);
int  bp_aliyun_auth_start(void);     // 0=已开始 1=已授权 -2=无网络 -3=忙
void bp_aliyun_auth_cancel(void);
void bp_aliyun_get_auth(char *url, size_t ucap, char *code, size_t ccap);
int  bp_aliyun_logout(void);         // 清除本地令牌;0 成功
void bp_aliyun_on_wifi(bool up);
// 通过串口写入 refresh_token 并尝试换取 access_token。0=已绑定 1=已开始授权 -1=格式错误 -2=无网络
int  bp_aliyun_set_refresh_token(const char *rt);
// 扫码登录:设备屏幕显示二维码,手机阿里云盘 App 扫码后自动完成绑定。0=已开始。
int  bp_aliyun_qr_start(void);
// 读取当前二维码内容与扫码状态(0 获取中 1 等待扫码 2 已扫码待确认 3 已完成/失败)。
void bp_aliyun_qr_get(char *content, size_t cap, int *status);
// 串口诊断:手动指定活动盘 ID(并持久化)/ 读取当前活动盘与资源库盘 ID。
int  bp_aliyun_set_drive(const char *drive_id);
void bp_aliyun_drive_info(char *drive, size_t dcap, char *res, size_t rcap);

// 异步列表(UI 用):结果写入内部 bp_list_t,用 bp_aliyun_list_snapshot 读取。
int  bp_aliyun_list_request(const bp_list_req_t *req);   // 0=已派发 -1 未就绪 -3 忙
void bp_aliyun_list_snapshot(bp_list_t *out);
// 同步列表(播放器自动续播用,调用方须在工作任务中)。0 成功。
int  bp_aliyun_list_fetch(const bp_list_req_t *req, bp_list_t *out);
// 取文件下载直链。0 成功;out 为 malloc 字符串,调用方 free。
int  bp_aliyun_dlink(const char *file_id, char **out);

// ---- 播放器 ------------------------------------------------------------------
void bp_player_init(void);
// 以列表快照为播放列表,从 index 开始播放(只取可播放文件)。0=已派发。
int  bp_player_play_list(const bp_list_t *list, int index);
void bp_player_toggle_pause(void);   // 已停止或出错时重播当前曲目
// 浏览网盘前挂起播放:立即静音并拆除整条流水线(下载连接、解码器、任务栈)以腾出
// 连续内存;bp_player_toggle_pause() 从挂起点续播。返回此前是否正在出声。
bool bp_player_suspend(void);
// 等待挂起完成、流水线内存释放(工作任务中调用),最多 timeout_ms。
void bp_player_wait_released(int timeout_ms);
void bp_player_next(void);
void bp_player_prev(void);
void bp_player_stop(void);
void bp_player_play_last(void);
bool bp_player_last_label(char *out, size_t cap);  // 上次播放的目录名(全部音频时为“全部音频”);无记录返回 false     // 开机续播:从上次播放的目录(和曲目)开始;没有记录时不动作
void bp_player_set_volume(uint8_t volume);
void bp_player_get_info(bp_player_info_t *out);
bool bp_player_active(void);        // 有曲目在缓冲/播放/暂停

// 网盘授权成功(由 bp_aliyun 授权任务调用,只投递事件)。
void bp_app_on_authorized(void);

// ---- 串口与测试钩子 ----------------------------------------------------------
void bp_console_start(void);
int  bp_test_key(int button, int kind);   // button 0 UP/1 OK/2 DOWN; kind 0 单击/1 长按/2 双击
int  bp_test_page(void);

// ---- Wi-Fi 桥接 --------------------------------------------------------------
#include "bp_wifi.h"
