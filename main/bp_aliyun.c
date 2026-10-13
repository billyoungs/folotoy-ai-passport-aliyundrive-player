// main/bp_aliyun.c —— 阿里云盘接入:refresh_token 直登 + 目录/音频列表 + 下载直链。
//
// 采用阿里云盘非官方 Web API(与 aligo/aliyunpan 社区库同一套):
//   令牌交换: POST https://api.aliyundrive.com/v2/account/token
//             body {"refresh_token":"...","grant_type":"refresh_token"}
//             响应含 access_token / refresh_token(轮换) / default_drive_id
//   目录列表: POST https://api.aliyundrive.com/adrive/v3/file/list?jsonmask=...
//             body {"drive_id","parent_file_id","limit","marker","order_by","order_direction"}
//   音频扫描: POST https://api.aliyundrive.com/v2/file/scan
//             body {"drive_id","category":"audio","limit","marker"}
//   下载直链: POST https://api.aliyundrive.com/v2/file/get_download_url
//             body {"drive_id","file_id","expire_sec":14400}
//
// 认证方式:设备上不实现扫码(非官方扫码需模拟安卓客户端、x-signature、Cookie 会话,
// 在 ESP32-C3 上复杂且易失效)。改为用户先在电脑上用 aligo / aliyunpan 等工具扫码
// 拿到 refresh_token,再通过 USB 串口命令 "ALI TOKEN <refresh_token>" 写入设备。
// 令牌过期(HTTP 401)时在互斥锁内刷新,刷新被拒则清空本地授权。
//
// 请求头: Authorization 为裸 access_token(无 Bearer 前缀);x-signature 为社区
// 逆向出的固定常量;x-device-id 首次随机生成并持久化。这些非官方值随时可能失效。
#include "bp_app.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "bp_ali"

#define ALI_NVS_NS "bp_ali"
// 认证专用域名:网页版真实 token 请求走 auth.aliyundrive.com,而非业务 API 域名。
// api.aliyundrive.com 的 token 接口被 WAF 重点风控(对 ESP32 的 TLS 指纹直接 RST)。
#define ALI_AUTH_HOST "https://auth.aliyundrive.com"
#define ALI_API_HOST "https://api.aliyundrive.com"
// 无 PSRAM 的 C3 堆极紧(空闲约 75 KiB):TLS 峰值约 30 KiB,叠加 cJSON 解析树与
// LVGL 显存曾把最低堆压到 472 字节,导致字形缓冲分配失败触发 LVGL while(1) 假死。
// 因此响应缓冲和每块条目数都取小值,一页 12 条由 1~2 块拼成。
// 列表响应缓冲:去掉 jsonmask 后服务器返回完整字段,一页 8 条响应约 7KB。
// v0.10.21 起用 7KB(原 8KB):播放环形缓冲 12→14KB 多占 2KB .bss,这里省回 1KB,
// 再配合列表最小连续块阈值 28→26KB,保持列表 TLS 握手不被挤出。
#define ALI_LIST_RESP_MAX (7 * 1024)
#define ALI_META_RESP_MAX 4608   // dlink 响应含带签名下载 URL(实测 ~2.8KB 且随文件波动),留足余量
#define ALI_AUTH_STACK 12288
#define ALI_LIST_STACK 8192
#define ALI_SCAN_LIMIT 4         // 音频扫描每块条数(实测 8 条响应 ~12.5KB,会超出 8KB 响应缓冲)
// 播放中再开一条 TLS 连接需要的最小连续内存(16 KiB 入站记录 + 余量)。
#define ALI_MIN_BLOCK_FOR_TLS (20 * 1024)

// 阿里云盘网页版前端应用 ID(公开常量,见浏览器抓包/社区库)。token 交换必须带上。
#define ALI_WEB_APP_ID "pJZInNHN2dZWk8qg"

// ---- 扫码登录(手机 App 扫设备屏幕二维码)----
// 参考 aligo / qrcode-token 社区实现:
//  1) GET /v2/oauth/authorize 种下 SESSIONID Cookie(浏览器会话入口)
//  2) GET /newlogin/qrcode/generate.do?appName=aliyun_drive 返回 codeContent(二维码)
//     与轮询所需的 t/ck 字段
//  3) POST /newlogin/qrcode/query.do?appName=aliyun_drive 轮询(t/ck 表单)
//     qrCodeStatus: NEW→SCANED(已扫码)→CONFIRMED(已确认)
//  4) CONFIRMED 后 content.data.bizExt 是 base64(gb18030) JSON,
//     内含 pds_login_result.refreshToken, 走现有 token 交换完成绑定
#define ALI_PASSPORT_HOST "https://passport.aliyundrive.com"
#define ALI_QR_CLIENT_ID "25dzX3vbYqktVxyX"
#define ALI_QR_APP_NAME  "aliyun_drive"
#define ALI_QR_RESP_MAX  4096

// 非官方客户端标识(社区逆向常量,见 aligo core/Auth.py)。当前已不再使用,
// 业务接口统一走浏览器头;保留定义以便需要时切换回安卓客户端模式。
#define ALI_CLIENT_UA "AliApp(AYSD/5.8.0) com.alicloud.databox/37029260 Channel/36176927979800@rimet_android_5.8.0 language/zh-CN /Android Mobile/Xiaomi Redmi"
#define ALI_X_SIGNATURE "f4b7bed5d8524a04051bd2da876dd79afe922b8205226d65855d02b267422adb1e0d8a816b021eaf5c36d101892180f79df655c5712b348c2a540ca136e6b22001"
#define ALI_X_CANARY "client=Android,app=adrive,version=v5.8.0"

// 请求头模式(完全对齐 aligo 社区库,它是大量用户验证可用的实现):
//   ALIGO = Referer + 安卓UA + x-canary,无 x-signature/无设备头 —— token 交换用;
//   APP   = ALIGO + x-device-id,业务接口(列表/扫描/下载)用;
//   WEB   = 浏览器头(已弃用,保留定义);
//   MINIMAL = 仅 Content-Type(已弃用,保留定义)。
// 注意:不再发送 x-signature(社区逆向常量已失效,且 aligo 也不需要)。
typedef enum {
    ALI_HDR_ALIGO = 0,
    ALI_HDR_MINIMAL,
    ALI_HDR_WEB,
    ALI_HDR_APP,
} ali_hdr_mode_t;

static SemaphoreHandle_t s_tok_lock;
// access_token 是较长的 JWT(实测网页版令牌响应整体 >2 KiB),512 装不下会触发长度
// 校验失败;放大到 2 KiB。令牌交换响应缓冲统一用 ALI_TOKEN_RESP_MAX(堆分配)。
#define ALI_TOKEN_RESP_MAX 4096
static char s_access[2048];
static char s_refresh[160];
static char s_drive_id[80];
// 阿里云盘有"备份盘"(default_drive_id)与"资源库"(resource_drive_id)两个盘。
// 用户上传的音频常在资源库;默认盘扫空时自动切到资源库,切过后 s_drive_id 即资源库 ID。
static char s_resource_id[80];
static char s_device_id[40];
static volatile bp_bd_state_t s_state = BP_BD_NO_AUTH;

static volatile bool s_auth_running;
static volatile bool s_auth_cancel;

// ---- 扫码登录状态 ----
static bool s_qr_running;
static volatile bool s_qr_cancel;
static volatile int  s_qr_status;   // 0 获取中 1 等待扫码 2 已扫码待确认 3 结束
static char s_qr_content[512];      // 二维码内容(URL)
static char s_qr_t[64];              // 轮询表单 t
static char s_qr_ck[64];             // 轮询表单 ck
static char s_qr_cookie[96];         // SESSIONID

static portMUX_TYPE s_list_mux = portMUX_INITIALIZER_UNLOCKED;
static bp_list_t s_list;            // 异步列表结果
static bool s_list_running;
static bp_list_req_t s_list_pending_req;

// ---- 令牌持久化 ---------------------------------------------------------------
static void nvs_save_tokens(void) {
    nvs_handle_t h;
    if (nvs_open(ALI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "at", s_access);
    nvs_set_str(h, "rt", s_refresh);
    nvs_set_str(h, "drive", s_drive_id);
    nvs_set_str(h, "rid", s_resource_id);
    nvs_set_str(h, "dev", s_device_id);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_tokens(void) {
    s_access[0] = s_refresh[0] = s_drive_id[0] = s_resource_id[0] = 0;
    nvs_handle_t h;
    if (nvs_open(ALI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t l = sizeof(s_access);
    nvs_get_str(h, "at", s_access, &l);
    l = sizeof(s_refresh);
    nvs_get_str(h, "rt", s_refresh, &l);
    l = sizeof(s_drive_id);
    nvs_get_str(h, "drive", s_drive_id, &l);
    l = sizeof(s_resource_id);
    nvs_get_str(h, "rid", s_resource_id, &l);
    l = sizeof(s_device_id);
    nvs_get_str(h, "dev", s_device_id, &l);
    nvs_close(h);
    if (!s_device_id[0]) {
        // 首次运行:随机生成设备 ID 并持久化。
        uint8_t raw[16];
        for (int i = 0; i < 16; i++) raw[i] = (uint8_t)esp_random();
        const char *hex = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            s_device_id[i * 2] = hex[raw[i] >> 4];
            s_device_id[i * 2 + 1] = hex[raw[i] & 15];
        }
        s_device_id[32] = 0;
        nvs_save_tokens();
    }
}

static void copy_access(char *out, size_t cap) {
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    strlcpy(out, s_access, cap);
    xSemaphoreGive(s_tok_lock);
}

static void sntp_start_once(void) {
    if (esp_sntp_enabled()) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
}

// ---- HTTPS 工具 ------------------------------------------------------------------
// 按模式设置会话请求头。auth 为空时不带 Authorization。
//   WEB:   浏览器头(Origin/Referer/UA/Accept)——网页版令牌交换(auth 域名)使用,
//          与在 Edge 控制台 fetch 成功的请求一致。
//   ALIGO: Referer + 安卓UA + x-canary,无签名——安卓令牌交换(api 域名)兜底。
//   APP:   ALIGO + x-device-id——已授权后的业务接口(列表/扫描/下载)使用。
//   MINIMAL: 仅 Content-Type(弃用)。
#define ALI_WEB_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
static void set_session_headers(esp_http_client_handle_t c, const char *auth, ali_hdr_mode_t mode) {
    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (mode == ALI_HDR_ALIGO || mode == ALI_HDR_APP) {
        // 对齐 aligo:Referer + 安卓UA + x-canary;APP 额外带 x-device-id。
        // 不再发 x-signature(aligo 不需要,社区常量也已失效)。
        esp_http_client_set_header(c, "Referer", "https://aliyundrive.com");
        esp_http_client_set_header(c, "User-Agent", ALI_CLIENT_UA);
        esp_http_client_set_header(c, "x-canary", ALI_X_CANARY);
        if (mode == ALI_HDR_APP && s_device_id[0])
            esp_http_client_set_header(c, "x-device-id", s_device_id);
        // 客户端每个业务请求都带随机 x-request-id(UUID 样式)。部分接口缺失该头时
        // 会返回 200 空 body(实测 /adrive/v3/file/list 一直如此)。
        char rid[37];
        snprintf(rid, sizeof(rid), "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
                 (unsigned long)esp_random(),
                 (unsigned long)((esp_random() >> 16) & 0xffff),
                 (unsigned long)(((esp_random() >> 16) & 0x0fff) | 0x4000),
                 (unsigned long)(((esp_random() >> 16) & 0x3fff) | 0x8000),
                 (unsigned long)((esp_random() >> 16) & 0xffff),
                 (unsigned long)esp_random());
        esp_http_client_set_header(c, "x-request-id", rid);
        // istio-envoy 网关对"裸"客户端请求(缺 Accept/Accept-Language 等)可能
        // 返回 200 响应头后即中断连接(body 0 字节)。补全浏览器/客户端特征头。
        esp_http_client_set_header(c, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(c, "Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
        esp_http_client_set_header(c, "Accept-Encoding", "identity");
    } else if (mode == ALI_HDR_WEB) {
        esp_http_client_set_header(c, "Referer", "https://www.aliyundrive.com/");
        esp_http_client_set_header(c, "Origin", "https://www.aliyundrive.com");
        esp_http_client_set_header(c, "User-Agent", ALI_WEB_UA);
        esp_http_client_set_header(c, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(c, "Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    }
    // ALI_HDR_MINIMAL: 仅 Content-Type(弃用)。
    if (auth && auth[0]) esp_http_client_set_header(c, "Authorization", auth);
}

// POST 上下文:perform 模式下用 HTTP_EVENT_ON_DATA 累积响应体到外部缓冲。
typedef struct {
    char *resp;
    size_t cap;
    size_t len;
} post_ctx_t;

static esp_err_t post_evt(esp_http_client_event_t *evt) {
    post_ctx_t *ctx = evt->user_data;
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len) {
        size_t room = ctx->cap - 1 - ctx->len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if (n) {
            memcpy(ctx->resp + ctx->len, evt->data, n);
            ctx->len += n;
        }
        ctx->resp[ctx->len] = 0;
    }
    return ESP_OK;
}

// POST JSON。返回 0=HTTP 2xx 且 resp 完整;-1 传输失败;-2 响应超出 cap;>0 HTTP 状态码。
static int http_post_json(const char *url, const char *body, const char *auth,
                          char *resp, size_t cap, int timeout_ms, ali_hdr_mode_t mode) {
    resp[0] = 0;
    post_ctx_t ctx = { .resp = resp, .cap = cap, .len = 0 };
    // 声明 ALPN(http/1.1):让 TLS ClientHello 带 ALPN 扩展,更接近浏览器指纹,
    // 规避阿里云 WAF 对"无 ALPN 的老式 TLS 客户端"握手后立即重置。
    static const char *alpn_protos[] = { "http/1.1", NULL };
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,
        // 收发缓冲按实际需求取小:响应体通过事件累积到外部 resp,客户端内部无需大缓冲;
        // 但 rx 要容得下响应头(阿里云盘接口头约 1-2KB,4096 足够;此前 8192 在
        // 挂起+列表页渲染时堆最大块仅 23KB,分配 rx+tx+resp 超限导致 Allocation failed)。
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .alpn_protos = alpn_protos,
        .event_handler = post_evt,
        .user_data = &ctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    set_session_headers(c, auth, mode);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    if (body) esp_http_client_set_post_field(c, body, (int)strlen(body));
    int rc = -1;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    ESP_LOGW(TAG, "response status=%d bytes=%u", status, (unsigned)ctx.len);
    // 诊断:2xx 但 body 为空。
    if (status >= 200 && status < 300 && ctx.len == 0) {
        ESP_LOGW(TAG, "empty 2xx body: errno=%d complete=%d",
                 errno, esp_http_client_is_complete_data_received(c));
    }
    if (err == ESP_OK) {
        if (status >= 200 && status < 300) rc = (ctx.len > 0) ? 0 : -1;
        else rc = status;
    } else {
        // perform 返回错误(如 401/403 响应):esp_http_client_perform 对非 2xx 返回
        // ESP_ERR_HTTP_CONNECT 等,但状态码仍然可取。必须按状态码返回(而非 -1),
        // 否则上层永远看不到 401,令牌过期后无法触发刷新(实测 list 一直 rc=-1)。
        rc = (status >= 400 && status < 600) ? status : -1;
    }
    // 非 2xx 时打印响应体,便于确认具体错误码(如 RefreshTokenExpired/BadRequest)。
    if ((status < 200 || status >= 300) && ctx.len > 0)
        ESP_LOGW(TAG, "HTTP %d body: %.200s", status, resp);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (rc != 0 && rc != 401)
        ESP_LOGW(TAG, "http failed path=%.48s rc=%d", strstr(url, "/v2/") ?: url, rc);
    return rc;
}

// ---- 扫码登录 ------------------------------------------------------------------
// 从 Set-Cookie 头提取 name=value 部分(分号前),追加到 out(浏览器会话需要
// 多个 Cookie,如 SESSIONID/aliyungf_tc/cna 等,只带 SESSIONID 会被风控拦截)。
static void qr_capture_cookie(const char *set_cookie, char *out, size_t cap) {
    size_t n = 0;
    while (set_cookie[n] && set_cookie[n] != ';' && set_cookie[n] != ',' && n < cap - 1) n++;
    if (!n) return;
    size_t len = strlen(out);
    if (len + n + 2 >= cap) return;
    if (len) { strlcat(out, "; ", cap); len += 2; }
    memcpy(out + len, set_cookie, n);
    out[len + n] = 0;
}

typedef struct { char cookies[512]; } qr_ctx_t;

static esp_err_t qr_evt(esp_http_client_event_t *evt) {
    qr_ctx_t *ctx = evt->user_data;
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        !strcasecmp(evt->header_key, "Set-Cookie")) {
        qr_capture_cookie(evt->header_value, ctx->cookies, sizeof(ctx->cookies));
    }
    return ESP_OK;
}

// GET 并把 body 累积到 resp(浏览器式请求头,可选 Cookie)。0 成功。
static int qr_get_json(const char *url, char *resp, size_t cap, const char *cookie) {
    resp[0] = 0;
    post_ctx_t ctx = { .resp = resp, .cap = cap, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = post_evt, .user_data = &ctx,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Referer", "https://aliyundrive.com");
    esp_http_client_set_header(c, "User-Agent", ALI_CLIENT_UA);
    esp_http_client_set_header(c, "x-canary", ALI_X_CANARY);
    if (cookie && cookie[0]) {
        esp_http_client_set_header(c, "Cookie", cookie);
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return (err == ESP_OK && status >= 200 && status < 300 && ctx.len > 0) ? 0 : -1;
}

// POST 表单并累积 body(浏览器式请求头,可选 Cookie)。0 成功。
static int qr_post_form(const char *url, const char *form, char *resp, size_t cap,
                        const char *cookie) {
    resp[0] = 0;
    post_ctx_t ctx = { .resp = resp, .cap = cap, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = post_evt, .user_data = &ctx,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(c, "Referer", "https://aliyundrive.com");
    esp_http_client_set_header(c, "User-Agent", ALI_CLIENT_UA);
    esp_http_client_set_header(c, "x-canary", ALI_X_CANARY);
    if (cookie && cookie[0]) {
        esp_http_client_set_header(c, "Cookie", cookie);
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_post_field(c, form, (int)strlen(form));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return (err == ESP_OK && status >= 200 && status < 300 && ctx.len > 0) ? 0 : -1;
}

// 从 cJSON 的 content.data 中取字符串字段(兼容新旧大小写)。
static const char *qr_data_str(const cJSON *root, const char *key) {
    const cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsObject(content)) return NULL;
    const cJSON *data = cJSON_GetObjectItem(content, "data");
    if (!cJSON_IsObject(data)) return NULL;
    const cJSON *v = cJSON_GetObjectItem(data, key);
    if (!cJSON_IsString(v)) {
        // 兼容大写首字母(老接口返回 QrCodeStatus / BizExt 等)
        char alt[32];
        snprintf(alt, sizeof(alt), "%c%s", (key[0] >= 'a' && key[0] <= 'z') ? key[0] - 'a' + 'A' : key[0], key + 1);
        v = cJSON_GetObjectItem(data, alt);
    }
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// 从 base64(gb18030) 的 bizExt 中提取 refreshToken:gb18030 中文不是 UTF-8,
// cJSON 解析会失败,直接用字符串搜索定位 ASCII 的 refreshToken 字段。
static int qr_extract_refresh(const char *biz_b64) {
    if (!biz_b64 || !biz_b64[0]) return -1;
    size_t slen = strlen(biz_b64);
    unsigned char *buf = malloc(slen);   // 解码后 ≤ 输入长度
    if (!buf) return -1;
    size_t olen = 0;
    int rc = mbedtls_base64_decode(buf, slen, &olen, (const unsigned char *)biz_b64, slen);
    if (rc != 0) { free(buf); return -1; }
    buf[olen] = 0;
    const char *key = "\"refreshToken\":\"";
    char *p = strstr((char *)buf, key);
    if (p) {
        p += strlen(key);
        char *end = strchr(p, '"');
        if (end) *end = 0;
        if (p[0]) {
            strlcpy(s_refresh, p, sizeof(s_refresh));
            ESP_LOGI(TAG, "qr login: refresh_token extracted (%u chars)", (unsigned)strlen(s_refresh));
            rc = 0;
        }
    }
    free(buf);
    return rc;
}

static void qr_login_task(void *arg) {
    (void)arg;
    char *resp = malloc(ALI_QR_RESP_MAX);
    if (!resp) { s_qr_running = false; vTaskDelete(NULL); return; }
    int done = -1;
    s_qr_status = 0;
    s_qr_content[0] = 0;
    s_qr_cookie[0] = 0;

    // 1) OAuth 入口种 SESSIONID Cookie
    char auth_url[256];
    snprintf(auth_url, sizeof(auth_url),
            ALI_AUTH_HOST "/v2/oauth/authorize"
            "?login_type=custom&response_type=code"
            "&redirect_uri=https://www.aliyundrive.com/sign/callback"
            "&client_id=%s&state=%%7B%%22origin%%22%%3A%%22file%%3A%%2F%%2F%%22%%7D",
            ALI_QR_CLIENT_ID);
    qr_ctx_t ctx = {0};
    esp_http_client_config_t cfg = {
        .url = auth_url, .method = HTTP_METHOD_GET, .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = qr_evt, .user_data = &ctx,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "Referer", "https://aliyundrive.com");
        esp_http_client_set_header(c, "User-Agent", ALI_CLIENT_UA);
        esp_http_client_set_header(c, "x-canary", ALI_X_CANARY);
        (void)esp_http_client_perform(c);
        esp_http_client_cleanup(c);
    }
    if (ctx.cookies[0]) {
        strlcpy(s_qr_cookie, ctx.cookies, sizeof(s_qr_cookie));
        ESP_LOGI(TAG, "qr login: session cookies len=%u", (unsigned)strlen(s_qr_cookie));
    } else {
        ESP_LOGW(TAG, "qr login: no cookies from authorize");
    }

    // 2) 生成二维码
    if (s_qr_cookie[0]) {
        char gen_url[160];
        snprintf(gen_url, sizeof(gen_url), ALI_PASSPORT_HOST "/newlogin/qrcode/generate.do?appName=%s",
                 ALI_QR_APP_NAME);
        if (qr_get_json(gen_url, resp, ALI_QR_RESP_MAX, s_qr_cookie) == 0) {
            cJSON *root = cJSON_Parse(resp);
            const char *code = root ? qr_data_str(root, "codeContent") : NULL;
            const char *ck = root ? qr_data_str(root, "ck") : NULL;
            // t 字段是数字(如 1791863705490)而非字符串,单独兼容处理。
            char t_buf[32] = {0};
            if (root) {
                const cJSON *content = cJSON_GetObjectItem(root, "content");
                const cJSON *data = content ? cJSON_GetObjectItem(content, "data") : NULL;
                const cJSON *tj = data ? cJSON_GetObjectItem(data, "t") : NULL;
                if (cJSON_IsNumber(tj)) snprintf(t_buf, sizeof(t_buf), "%.0f", tj->valuedouble);
                else if (cJSON_IsString(tj)) strlcpy(t_buf, tj->valuestring, sizeof(t_buf));
            }
            if (code && code[0] && t_buf[0] && ck && ck[0]) {
                strlcpy(s_qr_content, code, sizeof(s_qr_content));
                strlcpy(s_qr_t, t_buf, sizeof(s_qr_t));
                strlcpy(s_qr_ck, ck, sizeof(s_qr_ck));
                ESP_LOGI(TAG, "qr login: code generated len=%u", (unsigned)strlen(s_qr_content));
                if (root) cJSON_Delete(root);

                // 3) 轮询扫码状态(每 3 秒,最长约 6 分钟)
                char qurl[160];
                snprintf(qurl, sizeof(qurl), ALI_PASSPORT_HOST "/newlogin/qrcode/query.do?appName=%s",
                           ALI_QR_APP_NAME);
                char form[160];
                snprintf(form, sizeof(form), "t=%s&ck=%s", s_qr_t, s_qr_ck);
                s_qr_status = 1;   // 等待扫码
                for (int i = 0; i < 120 && !s_qr_cancel && !s_auth_cancel; i++) {
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    if (qr_post_form(qurl, form, resp, ALI_QR_RESP_MAX, s_qr_cookie) != 0) continue;
                    cJSON *r2 = cJSON_Parse(resp);
                    const char *st = r2 ? qr_data_str(r2, "qrCodeStatus") : NULL;
                    if (!st) { if (r2) cJSON_Delete(r2); continue; }
                    if (!strcmp(st, "SCANED") || !strcmp(st, "SCANNED")) {
                        s_qr_status = 2;
                    } else if (!strcmp(st, "CONFIRMED")) {
                        const char *biz = r2 ? qr_data_str(r2, "bizExt") : NULL;
                        if (biz && qr_extract_refresh(biz) == 0) {
                            done = 0;   // 已拿到 refresh_token
                        } else {
                            ESP_LOGW(TAG, "qr login: CONFIRMED but no bizExt");
                        }
                        if (r2) cJSON_Delete(r2);
                        break;
                    } else if (strstr(st, "EXPIRED") || strstr(st, "CANCEL")) {
                        ESP_LOGW(TAG, "qr login: %s", st);
                        if (r2) cJSON_Delete(r2);
                        break;
                    }
                    if (r2) cJSON_Delete(r2);
                }
            } else {
                ESP_LOGW(TAG, "qr login: generate parse failed root=%d raw=%.240s",
                        root ? 1 : 0, resp);
                if (root) cJSON_Delete(root);
            }
        } else {
            ESP_LOGW(TAG, "qr login: generate http failed raw=%.240s", resp);
        }
    } else {
        ESP_LOGW(TAG, "qr login: no cookies");
    }

    // 4) 用 refresh_token 完成绑定
    if (done == 0 && s_refresh[0]) {
        s_qr_status = 3;
        ESP_LOGI(TAG, "qr login: completing token exchange");
        bp_aliyun_set_refresh_token(s_refresh);
    } else {
        s_qr_status = 3;
        if (s_state == BP_BD_WAIT_QR) s_state = BP_BD_NO_AUTH;
        ESP_LOGW(TAG, "qr login failed");
    }
    free(resp);
    s_qr_running = false;
    vTaskDelete(NULL);
}

int bp_aliyun_qr_start(void) {
    if (s_state == BP_BD_READY) return 1;
    if (!g_bp.wifi_up) return -2;
    if (s_qr_running) return 0;
    s_qr_running = true;
    s_qr_cancel = false;
    s_qr_status = 0;
    s_state = BP_BD_WAIT_QR;
    if (xTaskCreate(qr_login_task, "bp_ali_qr", ALI_AUTH_STACK, NULL, 4, NULL) != pdPASS) {
        s_qr_running = false;
        s_state = BP_BD_NO_AUTH;
        return -3;
    }
    return 0;
}

void bp_aliyun_qr_get(char *content, size_t cap, int *status) {
    if (content && cap) strlcpy(content, s_qr_content, cap);
    if (status) *status = s_qr_status;
}

// ---- 令牌交换/刷新 ----------------------------------------------------------------
// 用 access_token 调 /v2/user/drives 一次拿全两个盘的 ID:默认盘(备份盘)与资源库。
// 注意 /v2/user/get 的响应里没有 resource_drive_id 字段(见 aligo BaseUser 定义),
// 只有这里的 drive 列表能凭 drive_name 区分"备份盘"与"资源库"。
static int fetch_drive_id_locked(char *buf, size_t cap) {
    if (!s_access[0]) return -1;
    // body 必须是合法 JSON:空串会被服务器判 400 InvalidRequestJSONFormat,至少给 "{}"。
    // 注意路径是 /v2/drive/list_my_drives(aligo V2_DRIVE_LIST_MY_DRIVES),
    // /v2/user/drives 并不存在,会返回 404。
    int rc = http_post_json(ALI_API_HOST "/v2/drive/list_my_drives", "{}", s_access, buf, cap, 15000, ALI_HDR_APP);
    if (rc != 0) return rc;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return -1;
    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n; i++) {
            const cJSON *d = cJSON_GetArrayItem(items, i);
            const cJSON *id = cJSON_GetObjectItem(d, "drive_id");
            const cJSON *nm = cJSON_GetObjectItem(d, "drive_name");
            if (!cJSON_IsString(id) || !id->valuestring[0] ||
                strlen(id->valuestring) >= sizeof(s_drive_id)) continue;
            const char *name =
                cJSON_IsString(nm) && nm->valuestring ? nm->valuestring : "";
            ESP_LOGI(TAG, "drive[%d]: id=%s name=%s", i, id->valuestring, name);
            // 资源库:中文名"资源库"或英文名"resource"(部分账号盘名为英文,如
            // alibum/note_drive/Default/resource/sharedAlbum,绝不能用"非默认盘"兜底,
            // 否则会错选相册/笔记盘)。
            if (strstr(name, "资源") || strstr(name, "resource")) {
                strlcpy(s_resource_id, id->valuestring, sizeof(s_resource_id));
                continue;
            }
            // 默认盘:仅在为空时填(令牌响应的 default_drive_id 已足够权威;账号可能
            // 有多个同名"Default"盘,无条件覆盖反而会选错)。即便 NVS 里存了别的盘,
            // 列表为空时也会自动切到资源库兜底。
            if (!s_drive_id[0]) {
                const cJSON *def = cJSON_GetObjectItem(d, "default");
                const cJSON *role = cJSON_GetObjectItem(d, "owner");
                if ((cJSON_IsBool(def) && def->valueint) ||
                    (cJSON_IsString(role) && strcmp(role->valuestring, "owner") == 0) ||
                    strstr(name, "备份") || strstr(name, "Default"))
                    strlcpy(s_drive_id, id->valuestring, sizeof(s_drive_id));
            }
        }
    }
    int result = s_drive_id[0] ? 0 : -1;
    cJSON_Delete(root);
    return result;
}

// 串口诊断用:手动指定活动盘 ID 并持久化(绕过自动选择,便于逐盘验证列表)。
int bp_aliyun_set_drive(const char *drive_id) {
    if (!drive_id) return -1;
    while (*drive_id == ' ' || *drive_id == '\t') drive_id++;
    size_t len = strlen(drive_id);
    while (len && (drive_id[len - 1] == '\r' || drive_id[len - 1] == '\n' ||
                   drive_id[len - 1] == ' '))
        len--;
    if (!len || len >= sizeof(s_drive_id)) return -1;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    memcpy(s_drive_id, drive_id, len);
    s_drive_id[len] = 0;
    nvs_save_tokens();
    xSemaphoreGive(s_tok_lock);
    ESP_LOGI(TAG, "drive manually set to %s", s_drive_id);
    return 0;
}

// 串口诊断用:取出当前活动盘与资源库盘 ID。
void bp_aliyun_drive_info(char *drive, size_t dcap, char *res, size_t rcap) {
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    strlcpy(drive, s_drive_id, dcap);
    strlcpy(res, s_resource_id, rcap);
    xSemaphoreGive(s_tok_lock);
}

// 调用方持有 s_tok_lock。成功写入令牌并持久化返回 0;业务错误返回 -2。
static int token_exchange_locked(char *resp, size_t cap) {
    // refresh_token 有两种来源,对应两套交换方式。关键:HTTP 400(参数/类型不匹配)
    // 不会让令牌失效,只有 200 成功才轮换,因此下面依次尝试多种组合,任一成功即可。
    //  A) 网页版令牌(浏览器 localStorage):必须带 app_id + 浏览器头,走 auth 域名
    //     —— 与在 Edge 控制台 fetch 验证成功的请求完全一致;
    //  B) 安卓客户端令牌(aligo/扫码):不带 app_id + 安卓头,走 api 域名。
    char web_body[256];
    snprintf(web_body, sizeof(web_body),
             "{\"grant_type\":\"refresh_token\",\"app_id\":\"%s\",\"refresh_token\":\"%s\"}",
             ALI_WEB_APP_ID, s_refresh);
    char app_body[256];
    snprintf(app_body, sizeof(app_body),
             "{\"refresh_token\":\"%s\",\"grant_type\":\"refresh_token\"}", s_refresh);
    struct { const char *url; const char *body; ali_hdr_mode_t mode; const char *tag; } tries[] = {
        { ALI_AUTH_HOST "/v2/account/token", web_body, ALI_HDR_WEB,   "web/auth"   },
        { ALI_API_HOST  "/v2/account/token", web_body, ALI_HDR_WEB,   "web/api"    },
        { ALI_API_HOST  "/v2/account/token", app_body, ALI_HDR_ALIGO, "aligo/api"  },
        { "https://api.alipan.com/v2/account/token", web_body, ALI_HDR_WEB, "web/alipan" },
    };
    int rc = -2;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        rc = http_post_json(tries[i].url, tries[i].body, NULL, resp, cap, 15000, tries[i].mode);
        if (rc == 0) { ESP_LOGI(TAG, "token exchange ok via %s", tries[i].tag); break; }
        // 400=令牌类型/参数不匹配,-1=传输失败:都继续尝试下一种组合(不会轮换令牌)。
        ESP_LOGW(TAG, "token exchange via %s rc=%d; try next", tries[i].tag, rc);
    }
    if (rc != 0) return rc == -1 ? -1 : -2;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return -1;
    const cJSON *at = cJSON_GetObjectItem(root, "access_token");
    const cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    const cJSON *dd = cJSON_GetObjectItem(root, "default_drive_id");
    const cJSON *rd = cJSON_GetObjectItem(root, "resource_drive_id");
    int result = -2;
    if (cJSON_IsString(at) && at->valuestring[0] &&
        strlen(at->valuestring) < sizeof(s_access)) {
        strlcpy(s_access, at->valuestring, sizeof(s_access));
        if (cJSON_IsString(rt) && rt->valuestring[0])
            strlcpy(s_refresh, rt->valuestring, sizeof(s_refresh));
        // 若之前已把活动盘切到"资源库",刷新令牌后仍保持;否则用默认(备份)盘。
        bool keep_resource = s_resource_id[0] && s_drive_id[0] &&
                             strcmp(s_drive_id, s_resource_id) == 0;
        if (cJSON_IsString(rd) && rd->valuestring[0])
            strlcpy(s_resource_id, rd->valuestring, sizeof(s_resource_id));
        if (!keep_resource && cJSON_IsString(dd) && dd->valuestring[0])
            strlcpy(s_drive_id, dd->valuestring, sizeof(s_drive_id));
        // 释放令牌响应后再拉 drive_id,避免嵌套响应缓冲区相互干扰。
        cJSON_Delete(root);
        root = NULL;
        // 令牌交换响应未携带 default_drive_id(浏览器头模式下常见),补拉一次用户空间列表。
        if (!s_drive_id[0]) {
            ESP_LOGW(TAG, "token resp without default_drive_id; query /v2/user/drives");
            char buf[2048];
            int drc = fetch_drive_id_locked(buf, sizeof(buf));
            if (drc == 0) ESP_LOGI(TAG, "drive_id resolved=%s", s_drive_id);
            else ESP_LOGW(TAG, "drive_id lookup failed rc=%d", drc);
        }
        nvs_save_tokens();
        result = 0;
    }
    if (root) cJSON_Delete(root);
    return result;
}

// seen = 调用方失败时使用的 access_token。若已被其他任务刷新,直接返回成功。
static int token_refresh(const char *seen) {
    int rc = -1;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    if (seen && strcmp(seen, s_access) != 0 && s_access[0]) {
        rc = 0;
    } else if (s_refresh[0]) {
        // 响应可达 ~2.2 KiB+,8 KiB 任务栈上放不下 4 KiB 缓冲,改堆分配。
        char *resp = malloc(ALI_TOKEN_RESP_MAX);
        if (!resp) { rc = -1; }
        else {
            rc = token_exchange_locked(resp, ALI_TOKEN_RESP_MAX);
            free(resp);
        }
        // 刷新被拒(HTTP 非 2xx:refresh_token 过期/吊销)才需要重新绑定。
        if (rc != 0 && rc != -1) {
            s_access[0] = s_refresh[0] = s_drive_id[0] = 0;
            s_state = BP_BD_NO_AUTH;
            ESP_LOGW(TAG, "refresh rejected; re-binding required");
        } else if (rc == -1) {
            ESP_LOGW(TAG, "refresh network failed; keep old tokens");
        }
    }
    xSemaphoreGive(s_tok_lock);
    return rc;
}

// ---- 授权任务 ---------------------------------------------------------------------
// 用已保存的 refresh_token 换取 access_token(由 ALI TOKEN 命令或 auth_start 触发)。
// 成功后 drive_id 为空时补拉 /v2/user/drives(令牌交换响应可能省略该字段)。
static void auth_task(void *arg) {
    (void)arg;
    int rc = -1;
    char *resp = malloc(ALI_TOKEN_RESP_MAX);
    while (resp && rc != 0 && !s_auth_cancel) {
        if (!g_bp.wifi_up) {
            for (int s = 0; s < 5 && !s_auth_cancel; s++) vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        xSemaphoreTake(s_tok_lock, portMAX_DELAY);
        rc = token_exchange_locked(resp, ALI_TOKEN_RESP_MAX);
        if (rc == 0 && (!s_drive_id[0] || !s_resource_id[0])) {
            // 令牌交换通过但盘 ID 不全(令牌响应常省略 resource_drive_id):
            // 调 /v2/user/drives 一次拿全默认盘与资源库 ID,避免只扫备份盘而音频在资源库。
            ESP_LOGW(TAG, "post-token drive info incomplete; query /v2/user/drives");
            int urc = fetch_drive_id_locked(resp, ALI_TOKEN_RESP_MAX);
            if (urc == 0 || s_drive_id[0]) {
                ESP_LOGI(TAG, "drives: default=%s resource=%s", s_drive_id, s_resource_id);
                nvs_save_tokens();
            } else {
                ESP_LOGW(TAG, "drive info lookup failed rc=%d", urc);
            }
            rc = 0;   // 令牌本身有效,盘 ID 待 WiFi 上线 backfill 再补
        }
        xSemaphoreGive(s_tok_lock);
        if (rc == -2) {
            ESP_LOGW(TAG, "refresh_token rejected; re-enter ALI TOKEN");
            break;
        }
        if (rc != 0)
            for (int s = 0; s < 10 && !s_auth_cancel; s++) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    free(resp);
    if (rc == 0 && !s_auth_cancel) {
        s_state = BP_BD_READY;
        sntp_start_once();
        ESP_LOGI(TAG, "AUTH: authorized drive=%s", s_drive_id);
        bp_app_on_authorized();
    } else if (s_state == BP_BD_WAIT_CODE) {
        s_state = BP_BD_NO_AUTH;
    }
    s_auth_running = false;
    vTaskDelete(NULL);
}

int bp_aliyun_auth_start(void) {
    if (s_state == BP_BD_READY) return 1;
    if (!g_bp.wifi_up) return -2;
    if (s_auth_running) return s_auth_cancel ? -3 : 0;
    if (!s_refresh[0]) {
        // 还没有 refresh_token:改用设备屏幕扫码登录(手机阿里云盘 App)。
        return bp_aliyun_qr_start();
    }
    s_auth_cancel = false;
    s_auth_running = true;
    s_state = BP_BD_WAIT_CODE;
    if (xTaskCreate(auth_task, "bp_ali_auth", ALI_AUTH_STACK, NULL, 4, NULL) != pdPASS) {
        s_auth_running = false;
        s_state = BP_BD_NO_AUTH;
        return -3;
    }
    return 0;
}

int bp_aliyun_set_refresh_token(const char *rt) {
    if (!rt || !rt[0]) return -1;
    // drive_id 缺失也强制重新走令牌交换:令牌交换本身会补拉 drive_id。
    if (s_state == BP_BD_READY && strcmp(rt, s_refresh) == 0 && s_drive_id[0]) return 0;
    strlcpy(s_refresh, rt, sizeof(s_refresh));
    s_drive_id[0] = 0;
    s_resource_id[0] = 0;
    s_access[0] = 0;
    nvs_save_tokens();
    if (!g_bp.wifi_up) return -2;    // 已保存,联网后再授权
    if (s_auth_running) return 1;
    int rc = bp_aliyun_auth_start();
    return rc == -1 ? 1 : rc;
}

void bp_aliyun_auth_cancel(void) {
    if (s_auth_running) s_auth_cancel = true;
    if (s_qr_running) s_qr_cancel = true;
}

void bp_aliyun_get_auth(char *url, size_t ucap, char *code, size_t ccap) {
    url[0] = code[0] = 0;   // 阿里云盘无设备码流程,不再产生二维码内容
}

bp_bd_state_t bp_aliyun_state(void) { return s_state; }

int bp_aliyun_logout(void) {
    if (s_auth_running) return -3;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ALI_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) return -1;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    memset(s_access, 0, sizeof(s_access));
    memset(s_refresh, 0, sizeof(s_refresh));
    memset(s_drive_id, 0, sizeof(s_drive_id));
    memset(s_resource_id, 0, sizeof(s_resource_id));
    s_state = BP_BD_NO_AUTH;
    xSemaphoreGive(s_tok_lock);
    portENTER_CRITICAL(&s_list_mux);
    memset(&s_list, 0, sizeof(s_list));
    portEXIT_CRITICAL(&s_list_mux);
    ESP_LOGI(TAG, "logged out locally (cloud files untouched)");
    return 0;
}

// ---- 列表 -------------------------------------------------------------------------
// 发起网络请求前要求的最大连续空闲块:TLS(~20 KiB) + 响应缓冲(7KB)/cJSON 解析起步 +
// LVGL 列表页渲染的字形分配余量。低于此值就先等待,避免堆耗尽把 UI 拖进 while(1) 假死。
// v0.10.21 播放环形缓冲 12→14KB 后,列表请求时堆最大连续块约 26KB,阈值同步降到 26KB。
#define ALI_LIST_MIN_BLOCK (26 * 1024)
static bool memory_allows_request(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= ALI_LIST_MIN_BLOCK;
}

// 最多等待 ~30s 让堆回落到安全水位;超时后只要还够开一条 TLS(~20KB)就放行,
// 否则授权刚结束/播放挂起后的碎片期(实测最大连续块在 17KB 附近、自愈需
// 15~25s)会让列表永远等不到。30s 覆盖完整自愈窗口。
static bool wait_for_memory(void) {
    for (int i = 0; i < 60; i++) {
        if (memory_allows_request()) return true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= ALI_MIN_BLOCK_FOR_TLS) {
        ESP_LOGW(TAG, "memory below watermark (largest=%u), proceed anyway",
                 (unsigned)largest);
        return true;
    }
    return false;
}

// 把一批原始条目中可显示的追加到 out。返回本批实际消费的原始条目数:
// out 已满时停在第一个放不下的可显示条目上,下一页从这里继续。
static int append_entries(const cJSON *items, bp_list_t *out, bool *stopped,
                         const char *folder) {
    int n = cJSON_GetArraySize(items);
    *stopped = false;
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(items, i);
        const cJSON *id = cJSON_GetObjectItem(e, "file_id");
        const cJSON *name = cJSON_GetObjectItem(e, "name");
        const cJSON *type = cJSON_GetObjectItem(e, "type");
        const cJSON *size = cJSON_GetObjectItem(e, "size");
        if (!cJSON_IsString(id) || !id->valuestring[0] || !cJSON_IsString(name))
            continue;
        bool is_dir = cJSON_IsString(type) && strcmp(type->valuestring, "folder") == 0;
        // 非音频文件且不是文件夹时,除非可播放,否则不显示。
        if (!bp_media_listable(is_dir, name->valuestring)) continue;
        if (out->count >= BP_PAGE_SIZE) {
            *stopped = true;
            return i;
        }
        bp_file_t *f = &out->files[out->count++];
        strlcpy(f->file_id, id->valuestring, sizeof(f->file_id));
        f->size = cJSON_IsNumber(size) ? (uint64_t)size->valuedouble : 0;
        f->is_dir = is_dir;
        f->format = is_dir ? BP_FMT_UNKNOWN : (uint8_t)bp_media_format(name->valuestring);
        bp_name_shorten(name->valuestring, f->name, sizeof(f->name));
        if (folder) bp_name_shorten(folder, f->folder, sizeof(f->folder));
        else strlcpy(f->folder, "全部音频", sizeof(f->folder));
    }
    return n;
}

// 请求一批原始条目(从 marker 起)并追加。返回 0 成功;*api_more 表示接口还有下一页。
static int fetch_chunk(const bp_list_req_t *req, const char *marker, bp_list_t *out,
                       char *resp, char *url, size_t url_cap, char *body,
                       size_t body_cap, bool *api_more) {
    // access_token(JWT)可达 ~2 KiB,不能放 8 KiB 任务栈,改堆分配。
    char *access = malloc(sizeof(s_access));
    if (!access) return -1;
    int result = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        copy_access(access, sizeof(s_access));
        if (!access[0]) { result = -1; break; }
        if (req->source == BP_SRC_ALL_AUDIO) {
            snprintf(url, url_cap,
                      ALI_API_HOST "/v2/file/scan");
            snprintf(body, body_cap,
                     "{\"drive_id\":\"%s\",\"category\":\"audio\",\"limit\":%d,\"marker\":\"%s\"}",
                     s_drive_id, ALI_SCAN_LIMIT, marker);
        } else {
            snprintf(url, url_cap,
                      ALI_API_HOST "/adrive/v3/file/list");
            const char *pid = req->parent_file_id[0] ? req->parent_file_id : "root";
            snprintf(body, body_cap,
                     "{\"drive_id\":\"%s\",\"parent_file_id\":\"%s\",\"limit\":%d,\"marker\":\"%s\",\"order_by\":\"name\",\"order_direction\":\"ASC\"}",
                     s_drive_id, pid, BP_PAGE_SIZE, marker);
        }
        // 资源库文件多,服务器组列表可能明显慢于小盘,超时取 30s(实测 15s 会读超时)。
        // 目录浏览用安卓客户端 APP 头(与 scan 一致):服务器对自家客户端身份最信任,
        // 网页版浏览器 UA 反而触发 WAF 返回 200 空 body。请求头截断问题已由
        // buffer_size_tx=4096 解决(见 http_post_json)。
        int rc = http_post_json(url, body, access, resp, ALI_LIST_RESP_MAX, 30000, ALI_HDR_APP);
        if (rc == -2) { result = -2; break; }
        if (rc == 401 && attempt == 0) {
            if (token_refresh(access) == 0) {
                // 刷新刚跑完一轮 TLS+cJSON,堆被压紧;等水位回落再重试,
                // 否则新连接的 TLS 缓冲分配会失败。
                wait_for_memory();
                continue;
            }
            break;
        }
        if (rc != 0) {
            // 诊断:失败时把已读到的原始 body 打出来。此前"30 字节空响应"的真实
            // 内容从未验证过——可能空 items,也可能是风控页/错误 JSON。
            ESP_LOGW(TAG, "list http rc=%d raw=%.200s", rc, resp);
            break;
        }
        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            ESP_LOGW(TAG, "list parse failed raw=%.200s", resp);
            break;
        }
        const cJSON *items = cJSON_GetObjectItem(root, "items");
        if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0)
            ESP_LOGI(TAG, "empty items raw=%.200s", resp);
        bool stopped = false;
        // 目录浏览时显示当前目录名;全部音频时按条目自身无路径,统一用"全部音频"。
        const char *folder = req->source == BP_SRC_DIR ?
            (strcmp(req->dir, "/") ? bp_path_basename(req->dir) : "我的网盘") : NULL;
        (void)append_entries(items, out, &stopped, folder);
        const cJSON *nm = cJSON_GetObjectItem(root, "next_marker");
        const char *next = cJSON_IsString(nm) ? nm->valuestring : "";
        bool chunk_more = next && next[0];
        *api_more = stopped || chunk_more;
        result = 0;
        cJSON_Delete(root);
        break;
    }
    free(access);
    return result;
}

// 从 req->marker 起向后扫描,直到凑满一页可显示条目、接口没有更多,或达到扫描上限。
#define ALI_MAX_SCAN_CHUNKS 6

int bp_aliyun_list_fetch(const bp_list_req_t *req, bp_list_t *out) {
    memset(out, 0, sizeof(*out));
    out->req = *req;
    out->status = -1;
    out->next_marker[0] = 0;
    if (!g_bp.wifi_up || s_state != BP_BD_READY) return -1;
    // 大块分配 + TLS 前先等堆回落到安全水位,避开 LVGL 重绘/播放的瞬时峰值。
    if (!wait_for_memory()) return -5;
    char *resp = malloc(ALI_LIST_RESP_MAX);
    char *url = malloc(256);
    char *body = malloc(1024);
    if (!resp || !url || !body) {
        free(resp);
        free(url);
        free(body);
        return -4;
    }
    char marker[BP_MARKER_MAX];
    strlcpy(marker, req->marker, sizeof(marker));
    bool api_more = true;
    int result = 0;
    // 根层级(全部音频 或 网盘根目录)第一页:
    // 当前盘(默认=备份盘)扫空且存在不同的资源库盘时,切到资源库再扫一次。
    bool at_root = req->source == BP_SRC_ALL_AUDIO ||
                   !req->parent_file_id[0] ||
                   !strcmp(req->parent_file_id, "root");
    int drive_attempts = 0;
retry_drive:
    out->count = 0;
    // 仅"切到资源库盘后重扫"才清空游标从头开始;翻页请求必须保留 req->marker,
    // 否则每次翻页都从第一页返回(实测 page=1 与 page=0 内容完全相同)。
    if (drive_attempts > 0) marker[0] = 0;
    api_more = true;
    result = 0;
    for (int chunk = 0; chunk < ALI_MAX_SCAN_CHUNKS && api_more && out->count < BP_PAGE_SIZE;
         chunk++) {
        bool chunk_more = false;
        int rc = fetch_chunk(req, marker, out, resp, url, 256, body, 1024, &chunk_more);
        if (rc != 0) {
            if (chunk == 0) result = rc;
            else api_more = true;
            break;
        }
        // 记录下一页游标:若因满页停止,从当前响应继续往后翻。
        cJSON *root = cJSON_Parse(resp);
        const cJSON *nm = root ? cJSON_GetObjectItem(root, "next_marker") : NULL;
        if (cJSON_IsString(nm) && nm->valuestring[0])
            strlcpy(marker, nm->valuestring, sizeof(marker));
        if (root) cJSON_Delete(root);
        api_more = chunk_more;
    }
    if (result == 0 && out->count == 0 && drive_attempts == 0 &&
        at_root && !req->marker[0] && s_resource_id[0] &&
        strcmp(s_resource_id, s_drive_id) != 0) {
        // 音频大概率在"资源库"盘:切换活动盘并持久化(下载直链也用该盘 ID)。
        drive_attempts++;
        xSemaphoreTake(s_tok_lock, portMAX_DELAY);
        ESP_LOGI(TAG, "no audio on drive %s; switching to resource drive %s",
                 s_drive_id, s_resource_id);
        strlcpy(s_drive_id, s_resource_id, sizeof(s_drive_id));
        nvs_save_tokens();
        xSemaphoreGive(s_tok_lock);
        goto retry_drive;
    }
    free(body);
    free(url);
    free(resp);
    if (result != 0) return result;
    strlcpy(out->next_marker, marker, sizeof(out->next_marker));
    out->has_more = api_more;
    out->status = 2;
    return 0;
}

static void list_task(void *arg) {
    (void)arg;
    bp_list_t *result = malloc(sizeof(bp_list_t));
    bp_list_req_t req;
    portENTER_CRITICAL(&s_list_mux);
    req = s_list_pending_req;
    portEXIT_CRITICAL(&s_list_mux);
    // 浏览前已自动挂起播放;等流水线拆除,把内存让给这次列表请求。
    if (bp_player_active()) bp_player_wait_released(3000);
    int rc = result ? bp_aliyun_list_fetch(&req, result) : -4;
    portENTER_CRITICAL(&s_list_mux);
    if (result) {
        s_list = *result;
    } else {
        s_list.status = -1;
    }
    s_list_running = false;
    portEXIT_CRITICAL(&s_list_mux);
    free(result);
    if (rc != 0)
        ESP_LOGW(TAG, "list failed rc=%d free=%u largest=%u", rc,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (rc == -5) bp_ui_toast("播放中内存不足，暂不能读取列表");
    else if (rc == -2) bp_ui_toast("目录过大，无法读取");
    else if (rc != 0) bp_ui_toast("读取网盘失败");
    vTaskDelete(NULL);
}

int bp_aliyun_list_request(const bp_list_req_t *req) {
    if (!req || req->page < 0 || req->page > 10000) return -1;
    if (!g_bp.wifi_up || s_state != BP_BD_READY) return -1;
    portENTER_CRITICAL(&s_list_mux);
    if (s_list_running) {
        portEXIT_CRITICAL(&s_list_mux);
        return -3;
    }
    s_list_running = true;
    s_list_pending_req = *req;
    s_list.req = *req;
    s_list.status = 1;
    s_list.count = 0;
    s_list.has_more = false;
    s_list.next_marker[0] = 0;
    portEXIT_CRITICAL(&s_list_mux);
    BaseType_t created = xTaskCreate(list_task, "bp_ali_list", ALI_LIST_STACK, NULL, 4, NULL);
    if (created != pdPASS && bp_player_active()) {
        bp_player_wait_released(3000);
        created = xTaskCreate(list_task, "bp_ali_list", ALI_LIST_STACK, NULL, 4, NULL);
    }
    if (created != pdPASS) {
        ESP_LOGW(TAG, "list task create failed free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        portENTER_CRITICAL(&s_list_mux);
        s_list_running = false;
        s_list.status = -1;
        portEXIT_CRITICAL(&s_list_mux);
        return -4;
    }
    return 0;
}

void bp_aliyun_list_snapshot(bp_list_t *out) {
    portENTER_CRITICAL(&s_list_mux);
    *out = s_list;
    portEXIT_CRITICAL(&s_list_mux);
}

// ---- 下载直链 -----------------------------------------------------------------------
int bp_aliyun_dlink(const char *file_id, char **out) {
    *out = NULL;
    if (!g_bp.wifi_up || s_state != BP_BD_READY) return -1;
    if (!file_id || !file_id[0]) return -1;
    char *resp = malloc(ALI_META_RESP_MAX);
    if (!resp) return -4;
    // access_token(JWT)可达 ~2 KiB,放播放器任务栈有溢出风险,改堆分配。
    char *access = malloc(sizeof(s_access));
    if (!access) { free(resp); return -4; }
    int result = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        copy_access(access, sizeof(s_access));
        if (!access[0]) break;
        char body[320];
        snprintf(body, sizeof(body),
                 "{\"drive_id\":\"%s\",\"file_id\":\"%s\",\"expire_sec\":14400}",
                 s_drive_id, file_id);
        int rc = http_post_json(ALI_API_HOST "/v2/file/get_download_url", body, access,
                                resp, ALI_META_RESP_MAX, 15000, ALI_HDR_APP);
        if (rc == 401 && attempt == 0) {
            if (token_refresh(access) == 0) continue;
            break;
        }
        if (rc != 0) break;
        cJSON *root = cJSON_Parse(resp);
        if (!root) break;
        const cJSON *u = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(u) && !strncmp(u->valuestring, "https://", 8)) {
            *out = strdup(u->valuestring);
            result = *out ? 0 : -4;
        } else {
            const cJSON *code = cJSON_GetObjectItem(root, "code");
            ESP_LOGW(TAG, "get_download_url code=%s", cJSON_IsString(code) ? code->valuestring : "?");
        }
        cJSON_Delete(root);
        break;
    }
    free(access);
    free(resp);
    return result;
}

void bp_aliyun_init(void) {
    s_tok_lock = xSemaphoreCreateMutex();
    nvs_load_tokens();
    s_state = s_refresh[0] ? BP_BD_READY : BP_BD_NO_AUTH;
    if (s_state == BP_BD_READY)
        ESP_LOGI(TAG, "stored authorization found drive=%s", s_drive_id);
    else if (s_refresh[0])
        ESP_LOGI(TAG, "refresh_token stored but access_token expired; re-auth on demand");
}

// WiFi 上线后在后台静默重拉盘列表(不改变 s_state,不触发 UI 通知)。无条件执行:
// NVS 里可能存有旧固件写入的错误 resource_id(如把相册盘当资源库),只有以服务器
// 的盘列表为准重新推导才能纠正。
static void drive_backfill_task(void *arg) {
    (void)arg;
    for (int wait = 0; wait < 10 && !g_bp.wifi_up; wait++)
        vTaskDelay(pdMS_TO_TICKS(1000));
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    if (g_bp.wifi_up && s_access[0]) {
        char *buf = malloc(ALI_TOKEN_RESP_MAX);
        if (buf) {
            ESP_LOGI(TAG, "backfill drive info; query /v2/drive/list_my_drives");
            int brc = fetch_drive_id_locked(buf, ALI_TOKEN_RESP_MAX);
            if (brc == 0 || s_drive_id[0]) {
                ESP_LOGI(TAG, "drives: default=%s resource=%s", s_drive_id, s_resource_id);
                nvs_save_tokens();
            } else {
                ESP_LOGW(TAG, "drive info backfill failed rc=%d", brc);
            }
            free(buf);
        }
    }
    xSemaphoreGive(s_tok_lock);
    vTaskDelete(NULL);
}

void bp_aliyun_on_wifi(bool up) {
    if (up) sntp_start_once();
    // 每次联网都以服务器盘列表刷新盘 ID(纠正 NVS 中的陈旧/错误值,如旧固件
    // 把相册盘误存为资源库),后台执行避免阻塞 WiFi 事件回调。
    if (up && s_state == BP_BD_READY && s_access[0] &&
        xTaskCreate(drive_backfill_task, "bp_ali_bfill",
                    ALI_AUTH_STACK, NULL, 4, NULL) == pdPASS) {
        ESP_LOGI(TAG, "scheduled drive info backfill");
    }
}