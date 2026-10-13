// main/bp_player.c —— 流式播放引擎。
//
// 资源归属(每首歌一条流水线,同一时刻只有一条):
//   ctrl 任务(常驻)  串行处理播放/切歌/停止命令,获取下载直链,启动/回收流水线
//   fetch 任务       HTTPS 下载 → StreamBuffer;跟随 302,断线按 Range 续传
//   decode 任务      StreamBuffer → MP3(Helix)/WAV 解码 → 单声道 PCM → I2S
// decode 任务是流水线的持有者:它启动 fetch、等待 fetch 退出、释放缓冲,最后把
// 结束/失败事件(带代号)投回 ctrl,ctrl 忽略过期代号的事件。
// 按键回调只投递命令或改暂停标志,不做任何网络/音频操作。
#include "bp_app.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "aacdec.h"
#include "bp_mp4.h"
#include "mp3dec.h"
#include "nvs.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "bp_play"

#define CTRL_STACK 7168
#define FETCH_STACK 7168
#define DECODE_STACK 6144
// 下载每次读取 4KB:TLS 记录(16KB)分 4 次读完,比 2KB 时系统调用/调度次数减半,
// 让 320kbps(40KB/s)下环形缓冲回填更及时。
#define FETCH_CHUNK 4096
#define FETCH_MAX_FAILURES 6
// 输入缓冲 2048:恰好容纳最大 MP3 帧(320kbps@44.1kHz 单帧 1447B + 帧头)与
// MAINBUF_SIZE(1940) 窗口,也满足 AAC 解码的最小缓冲(AAC_MIN_AVAIL)。再小会
// 影响 MP3 帧装配,再大则白占 .bss(.bss 直接挤占堆池,见 STREAM_SIZE 注释)。
#define IN_BUF_SIZE 2048
#define PCM_MAX_SAMPLES (1152 * 2)
// 环形缓冲与解码缓冲静态分配:若从堆里取,会切走唯一一块能容纳 TLS 16.7 KB
// 记录缓冲的连续内存(实测 CDN 读数据时 alloc 失败;v0.10.20 曾尝试播放时从堆
// 分配 24KB 大缓冲,结果 TLS 握手 alloc(4781) 失败、最低堆 412B,必须回退静态)。
// 流缓冲放 .bss(静态 RAM):.bss 每多 1KB,DRAM 堆池就少 1KB,而 CDN TLS 握手
// 峰值要吃 ~67KB 堆,堆池过小握手必败。14KB ≈ 0.35s@320kbps,与列表请求内存
// (响应缓冲 8→7KB)做了折中:v0.10.21 起保持 14KB,不再增大以免列表 TLS 失败。
#define STREAM_SIZE (14 * 1024)
#define MAX_DECODE_ERRORS 300

typedef enum {
    CMD_PLAY,
    CMD_PLAY_LAST,
    CMD_NEXT,
    CMD_PREV,
    CMD_STOP,
    CMD_REPLAY,
    CMD_SUSPEND,
    CMD_RESUME,
    CMD_TRACK_END,
    CMD_TRACK_FAIL,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t gen;
    const char *error;
} cmd_t;

typedef struct {
    char *url;
    uint64_t size;
    uint32_t gen;
    uint64_t start;    // 从文件的这个偏移开始下载(续播时非 0)
    bool resume;       // 续播:跳过文件头探测,沿用挂起时的格式与进度
} track_t;

// 挂起点:decode 任务退出时记录;浏览网盘时整条流水线拆除以腾出连续内存,
// 回到播放页按 OK 后从这里用 Range 续播。
typedef struct {
    uint64_t offset;   // 下一个未解码字节在文件中的偏移
    uint32_t pos_ms;
    uint32_t total_ms;
    bp_sniff_t kind;   // 续播用哪个解码器
    bp_wav_info_t wav;
    bp_m4a_info_t m4a; // 裸 AAC 块没有同步字,续播必须沿用解析出的参数
} resume_t;

static QueueHandle_t s_cmd;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// 播放列表(只含可播放文件)与其来源,用于末尾自动加载下一页。
static bp_file_t s_playlist[BP_PLAYLIST_MAX];
static int s_pl_count;
static int s_pl_index;
static bp_list_req_t s_pl_req;
static bool s_pl_has_more;
static char s_pl_next_marker[BP_MARKER_MAX];
static bp_file_t s_pending_list[BP_PLAYLIST_MAX];   // CMD_PLAY 携带的数据
static int s_pending_count, s_pending_index;
static bp_list_req_t s_pending_req;
static bool s_pending_has_more;
static char s_pending_next_marker[BP_MARKER_MAX];

static bp_player_info_t s_info;
static volatile bool s_paused;
static volatile bool s_abort;
static volatile bool s_pipeline_running;
static volatile bool s_fetch_running;
static volatile bool s_fetch_eof;
static volatile bool s_fetch_failed;
static volatile bool s_fetch_parked;    // 暂停时已关闭下载连接,等待继续
static volatile bool s_suspended;       // 流水线已拆除,等待续播
static resume_t s_resume;
static uint32_t s_base_ms;              // 本条流水线起播时的进度(续播时非 0)
static bp_sniff_t s_cur_kind;
static bp_wav_info_t s_cur_wav;
static bp_m4a_info_t s_cur_m4a;
static uint32_t s_gen;
static StreamBufferHandle_t s_stream;
static const size_t s_stream_size = STREAM_SIZE;
static StaticStreamBuffer_t s_stream_struct;
static uint8_t s_stream_storage[STREAM_SIZE + 1];
static uint8_t s_in_storage[IN_BUF_SIZE] __attribute__((aligned(4)));
static int16_t s_pcm_storage[PCM_MAX_SAMPLES];

static void log_heap(const char *stage) {
    ESP_LOGI(TAG, "MEM %s: free=%u largest=%u min=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size());
}

// ---- 堆竞技场 ------------------------------------------------------------------
// CDN 下载读 16KB TLS 记录需要 ~16.7KB 连续内存(动态缓冲按记录大小现场分配),
// 播放期的堆碎片化会让等大空闲块不复存在(实测 free 6 万而最大块仅 16KB,分配失败,
// 每首曲目都报网络错误)。预留 18KB"竞技场",播放全程(含获取下载直链)释放给
// TLS 使用,暂停/曲目结束/停止时归还。MP3 解码器已静态化(见 libhelix-mp3 的
// buffers.c 本地补丁),不再与 TLS 争连续内存。
#define HEAP_ARENA_SIZE (18 * 1024)
// probe 只要求"够放一条最大 TLS 记录"(16KB 内容 + ~0.4KB 记录开销 ≈ 16.7KB):
// 坚持要求整个 18KB 竞技场时,碎片化峰值期会等满 16 秒仍放弃(实测 largest 卡在
// 15.3KB),而其实 17KB 已够握手+读记录使用。
#define HEAP_ARENA_PROBE (17 * 1024)
static uint8_t *s_heap_arena;

static void heap_arena_release(const char *why) {
    if (s_heap_arena) {
        free(s_heap_arena);
        s_heap_arena = NULL;
    } else {
        // 开机后首播还没有预留块:开机阶段的多路 TLS 握手会暂时把堆搅碎,但这些
        // 临时分配在数秒内就归还合并(实测曲目失败后 ~15s,largest 恢复到 18K)。
        // 等待-重试直到拿到连续块,给开机自动续播留出堆自愈的时间。
        for (int i = 0; i < 8 && !s_abort; i++) {
            void *probe = malloc(HEAP_ARENA_PROBE);
            if (probe) {
                free(probe);   // 立即归还:把它放回堆顶供随后的 TLS 大记录分配
                break;
            }
            if (i == 0)
                ESP_LOGW(TAG, "waiting for a %uKB contiguous block (%s)...",
                         (unsigned)(HEAP_ARENA_PROBE / 1024), why);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    ESP_LOGI(TAG, "arena released (%s) free=%u largest=%u", why,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void heap_arena_restore(const char *why) {
    if (s_heap_arena) return;
    s_heap_arena = malloc(HEAP_ARENA_SIZE);
    ESP_LOGI(TAG, "arena %s (%s)", s_heap_arena ? "restored" : "restore failed", why);
}

// MP3 解码器跨曲目持久化:helix 初始化要分 8 次共 ~16KB 的堆分配,若在下载
// 连接存活时初始化必然失败(实测最大块只剩 5-7KB)。改为播放开始、竞技场已
// 释放、下载连接尚未建立的窗口内一次性初始化,之后所有曲目共用(helix 的帧间
// 状态只有 bit reservoir,换曲最多前几帧报 MAIN_DATA_UNDERFLOW,解码循环已处理)。
static HMP3Decoder s_mp3_dec;

static void mp3_ensure(void) {
    if (s_mp3_dec) return;
    s_mp3_dec = MP3InitDecoder();
    ESP_LOGI(TAG, "mp3 decoder %s", s_mp3_dec ? "ready (persistent)" : "init failed");
}

static void set_state(bp_play_state_t st, const char *error) {
    portENTER_CRITICAL(&s_mux);
    s_info.state = st;
    s_info.error = error;
    portEXIT_CRITICAL(&s_mux);
}

static void post(cmd_type_t type, uint32_t gen, const char *error) {
    cmd_t c = {.type = type, .gen = gen, .error = error};
    if (s_cmd) xQueueSend(s_cmd, &c, pdMS_TO_TICKS(100));
}

// ---- fetch 任务 -----------------------------------------------------------------
typedef struct {
    char *location;
} fetch_ctx_t;

static esp_err_t fetch_event(esp_http_client_event_t *evt) {
    fetch_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx && evt->header_key &&
        evt->header_value && !strcasecmp(evt->header_key, "Location") &&
        strlen(evt->header_value) < 4096) {
        free(ctx->location);
        ctx->location = strdup(evt->header_value);
    }
    return ESP_OK;
}

// 返回实际写入的字节数。中止,或暂停且缓冲已满时提前返回;未写入的部分由
// 调用方丢弃(不计入下载偏移),继续时按 Range 重新下载。
static size_t stream_send(const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len && !s_abort) {
        size_t got = xStreamBufferSend(s_stream, data + off, len - off, pdMS_TO_TICKS(100));
        off += got;
        if (!got && s_paused) break;
    }
    return off;
}

// CDN 明文 HTTP 探测:TLS 下载的内存代价(握手峰值 ~64K + 16.7K 记录缓冲)几乎
// 耗尽全部堆池,解码器与之无法共存(三代方案均失败,见 git 历史)。阿里 CDN 的
// 直链通常同时支持 80 端口,改走明文后内存矛盾整体消失。首轮下载先试 http,
// 一旦失败(4xx/网络错)本轮起永久回退 https,不再反复试探。
static bool s_prefer_http = true;

static void url_downgrade_http(char *url) {
    if (!url || strncmp(url, "https://", 8)) return;
    memmove(url + 4, url + 5, strlen(url) - 4);   // 去掉 "https" 里的 's':"http://..."
}

// 返回 1=完整下载结束,0=被中止,2=因暂停主动断开,-1=可重试错误,-2=不可重试(4xx 等)。
static int fetch_once(const char *start_url, uint64_t *offset, uint8_t *buf, bool prefer_http) {
    // TLS1.3 已在 sdkconfig 关闭(内存考量),CDN 连接改发 ALPN http/1.1 维持浏览器式
    // 指纹,规避阿里 WAF 对"仅 TLS1.2 + 无 ALPN"连接的风控(bp_ali 同款处理)。
    static const char *alpn_protos[] = { "http/1.1", NULL };
    char *url = strdup(start_url);
    if (prefer_http) url_downgrade_http(url);
    int result = -1;
    for (int hop = 0; url && hop < 5 && !s_abort; hop++) {
        fetch_ctx_t ctx = {0};
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .method = HTTP_METHOD_GET,
            // 明文探测大概率不通(实测 CDN 80 端口直接超时),用短超时把每轮
            // 开机的浪费控制在 4 秒内;https 正式下载维持 10 秒。
            .timeout_ms = prefer_http ? 4000 : 10000,
            .disable_auto_redirect = true,
            .event_handler = fetch_event,
            .user_data = &ctx,
            // rx 缓冲 4KB:配合 FETCH_CHUNK=4096,每次 read 返回一整块数据,
            // 减少 TLS 记录分片读取的次数(2KB 时读满 16KB 记录要 8 次)。
            .buffer_size = 4096,
            // dlink 带签名参数,请求行较长:实测阿里云盘 CDN 签名 URL 普遍 >2.5KB,
            // 2048 时 esp_http_client 组请求行报 "Out of buffer",下载必失败。
            .buffer_size_tx = 4096,
            .alpn_protos = alpn_protos,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) break;
        // 阿里云盘下载直链要求 Referer 头;用浏览器 UA 避免被风控识别为非浏览器客户端重置。
        esp_http_client_set_header(c, "Referer", "https://www.aliyundrive.com/");
        esp_http_client_set_header(c, "Origin", "https://www.aliyundrive.com");
        esp_http_client_set_header(c, "User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        char range[40];
        if (*offset) {
            snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", *offset);
            esp_http_client_set_header(c, "Range", range);
        }
        esp_err_t err = esp_http_client_open(c, 0);
        int status = 0;
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
        }
        if (err == ESP_OK && status >= 300 && status < 400 && ctx.location &&
            (!strncmp(ctx.location, "https://", 8) || !strncmp(ctx.location, "http://", 7))) {
            free(url);
            url = ctx.location;
            ctx.location = NULL;
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            continue;
        }
        free(ctx.location);
        if (err == ESP_OK && (status == 200 || status == 206)) {
            log_heap(hop ? "cdn-connected" : "connected");
            // 服务器忽略 Range 时从头读,丢弃已送出的部分。
            uint64_t skip = (status == 200) ? *offset : 0;
            int n;
            while (!s_abort && (n = esp_http_client_read(c, (char *)buf, FETCH_CHUNK)) > 0) {
                size_t start = 0;
                if (skip) {
                    size_t drop = skip > (uint64_t)n ? (size_t)n : (size_t)skip;
                    skip -= drop;
                    start = drop;
                }
                size_t want = (size_t)n - start;
                size_t sent = want ? stream_send(buf + start, want) : 0;
                *offset += sent;
                // 暂停时立刻释放连接(TLS 约 30 KB),让浏览网盘等请求有内存可用。
                if (sent < want || s_paused) break;
            }
            if (s_abort) result = 0;
            else if (esp_http_client_is_complete_data_received(c)) result = 1;
            else if (s_paused) result = 2;
            else result = -1;
        } else if (err == ESP_OK && status == 416) {
            result = 1;   // 续传偏移已到文件尾
        } else if (err == ESP_OK && status >= 400 && status < 500) {
            ESP_LOGW(TAG, "download http=%d", status);
            result = -2;
        } else {
            ESP_LOGW(TAG, "download open=%s http=%d", esp_err_to_name(err), status);
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        break;
    }
    free(url);
    return result;
}

static void fetch_task(void *arg) {
    track_t *t = arg;
    uint8_t *buf = malloc(FETCH_CHUNK);
    uint64_t offset = t->start;
    int failures = 0, rc = -1;
    while (buf && !s_abort) {
        uint64_t before = offset;
        rc = fetch_once(t->url, &offset, buf, s_prefer_http);
        if (rc != 2 && rc != 0 && s_prefer_http) {
            // 明文探测失败:换回 https(签名直链对 http 也可能被 403/重定向)。
            s_prefer_http = false;
            ESP_LOGW(TAG, "plain http download failed, falling back to https");
            continue;   // 立即用 https 重试,不计入 failures
        }
        if (rc == 2) {
            ESP_LOGI(TAG, "paused: download connection released at %" PRIu64, offset);
            s_fetch_parked = true;
            log_heap("parked");
            // 暂停/挂起期间不恢复 arena:18KB 预留块保持释放,浏览网盘/切页的列表
            // 请求才有足够连续内存(历史:挂起时 malloc 回 18KB 会把最大连续块切到
            // 9~17KB,列表请求 TLS 需 ≥20KB,导致"播放中内存不足" rc=-5)。
            while (s_paused && !s_abort) vTaskDelay(pdMS_TO_TICKS(100));
            s_fetch_parked = false;
            continue;   // 继续播放:按 Range 从断点重连
        }
        if (rc >= 0 || rc == -2) break;
        if (offset > before) failures = 0;   // 有进展就重新计数(如暂停后被服务器断开)
        if (++failures >= FETCH_MAX_FAILURES) break;
        ESP_LOGW(TAG, "download interrupted at %" PRIu64 ", retry %d", offset, failures);
        for (int i = 0; i < failures * 10 && !s_abort; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    free(buf);
    s_fetch_failed = rc < 0 && !s_abort;
    s_fetch_eof = true;
    s_fetch_running = false;
    vTaskDelete(NULL);
}

// ---- decode 任务 ----------------------------------------------------------------
typedef struct {
    uint8_t *in;
    size_t len;      // in 中有效字节(从 in[0] 开始)
    size_t pos;      // 已消费字节
    uint64_t received;   // 累计从流中读入的字节(用于计算挂起点)
} inbuf_t;

static void update_buffer_pct(void) {
    size_t avail = xStreamBufferBytesAvailable(s_stream);
    portENTER_CRITICAL(&s_mux);
    s_info.buffer_pct = (uint8_t)(avail * 100 / s_stream_size);
    portEXIT_CRITICAL(&s_mux);
}

// 等待缓冲到目标比例(或下载结束);期间状态为“缓冲中”。返回 false 表示中止。
static bool wait_buffered(int pct) {
    size_t want = s_stream_size * pct / 100;
    while (!s_abort && !s_fetch_eof && xStreamBufferBytesAvailable(s_stream) < want) {
        bp_play_state_t st = s_paused ? BP_PLAY_PAUSED : BP_PLAY_BUFFERING;
        if (s_info.state != st) set_state(st, NULL);
        update_buffer_pct();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return !s_abort;
}

static bool stream_done(void) {
    return s_fetch_eof && xStreamBufferIsEmpty(s_stream);
}

// 填充输入缓冲到满或流暂时为空(欠载时会先重新缓冲)。返回新增字节数;
// 返回 0 且 stream_done() 为真表示数据已全部读完。
static size_t refill(inbuf_t *b) {
    if (b->pos) {
        memmove(b->in, b->in + b->pos, b->len - b->pos);
        b->len -= b->pos;
        b->pos = 0;
    }
    size_t added = 0;
    while (b->len < IN_BUF_SIZE && !s_abort) {
        size_t got = xStreamBufferReceive(s_stream, b->in + b->len, IN_BUF_SIZE - b->len,
                                          pdMS_TO_TICKS(20));
        b->len += got;
        added += got;
        b->received += got;
        if (got) continue;
        if (stream_done() || b->len >= IN_BUF_SIZE / 2) break;
        // 欠载:重新缓冲 60% 再继续(原 25% 只够 0.09s,恢复后很快再次欠载,
        // 导致"缓冲中"频繁闪现)。60% ≈ 8.4KB@14KB = 0.21s@320kbps,恢复后
        // 缓冲更足,下一次欠载明显推迟,播放更连续。
        if (!wait_buffered(60)) break;
    }
    return added;
}

// 丢弃 n 字节(用于跳过 ID3 标签)。
static bool discard(inbuf_t *b, size_t n) {
    while (n && !s_abort) {
        size_t have = b->len - b->pos;
        if (!have) {
            if (!refill(b) && stream_done()) return false;
            continue;
        }
        size_t d = have < n ? have : n;
        b->pos += d;
        n -= d;
    }
    return !s_abort;
}

static void wait_while_paused(void) {
    if (!s_paused) return;
    portENTER_CRITICAL(&s_mux);
    s_info.level = 0;
    portEXIT_CRITICAL(&s_mux);
    set_state(BP_PLAY_PAUSED, NULL);
    while (s_paused && !s_abort) {
        update_buffer_pct();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_abort) set_state(BP_PLAY_PLAYING, NULL);
}

static bool open_codec(uint32_t rate) {
    if (bsp_audio_set_format(rate, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed at %lu Hz", (unsigned long)rate);
        return false;
    }
    bsp_audio_set_volume(g_bp.volume);
    return true;
}

// 实时电平:对已下混的单声道 PCM 抽样求平均幅度,上升快、回落慢,驱动界面声波。
static void publish_level(const int16_t *pcm, size_t n) {
    uint32_t acc = 0, cnt = 0;
    for (size_t i = 0; i < n; i += 8, cnt++) acc += (uint32_t)(pcm[i] < 0 ? -pcm[i] : pcm[i]);
    uint32_t lv = cnt ? acc / cnt * 100 / 6000 : 0;
    if (lv > 100) lv = 100;
    portENTER_CRITICAL(&s_mux);
    s_info.level = (uint8_t)(lv > s_info.level ? lv : (s_info.level * 7u + lv) / 8u);
    portEXIT_CRITICAL(&s_mux);
}

static void publish_progress(uint64_t frames, uint32_t rate, uint32_t total_ms) {
    portENTER_CRITICAL(&s_mux);
    if (s_info.state == BP_PLAY_BUFFERING) s_info.state = BP_PLAY_PLAYING;
    s_info.pos_ms = s_base_ms + bp_pcm_ms(frames, rate);
    if (total_ms) s_info.total_ms = total_ms;
    portEXIT_CRITICAL(&s_mux);
}

static const char *play_mp3(inbuf_t *b, uint64_t file_size, size_t id3) {
    mp3_ensure();   // 已初始化则复用;异常时序(竞技场未释放)下兜底再试一次
    HMP3Decoder dec = s_mp3_dec;
    log_heap("decoder-ready");
    int16_t *pcm = s_pcm_storage;
    const char *error = NULL;
    if (!dec) {
        error = "内存不足";
        goto out;
    }
    uint32_t rate = 0;
    uint64_t frames_out = 0, bitrate_sum = 0, bitrate_frames = 0;
    int errors = 0;
    // 首个解码成功的帧锁定版本与采样率;之后参数不符的“帧头”都视为误判,
    // 不再触发重开音频设备(那会导致长时间卡顿)。
    bp_mp3_hdr_t lock = {0}, hdr;
    bool locked = false;
    size_t skipped = 0;   // 连续找不到可信帧时丢弃的字节数
    while (!s_abort) {
        wait_while_paused();
        if (s_abort) break;
        if (b->len - b->pos < MAINBUF_SIZE && !stream_done()) refill(b);
        int left = (int)(b->len - b->pos);
        if (left <= 0) {
            if (stream_done()) break;
            continue;
        }
        size_t partial = 0;
        int sync = bp_mp3_find_frame(b->in + b->pos, (size_t)left, stream_done(),
                                     locked ? &lock : NULL, &hdr, &partial);
        if (sync == -2) {   // 候选帧的下一帧还没读到,补数据后再确认
            b->pos += partial;
            refill(b);
            continue;
        }
        if (sync < 0) {
            // 保留末尾 3 字节,帧头可能跨越缓冲边界。
            size_t drop = left > 3 ? (size_t)left - 3 : (size_t)left;
            b->pos += drop;
            skipped += drop;
            if (skipped > 256 * 1024) {
                error = frames_out ? "解码失败" : "不是有效的 MP3";
                break;
            }
            if (!refill(b) && stream_done()) break;
            continue;
        }
        b->pos += sync;
        left -= sync;
        skipped = 0;
        unsigned char *ptr = b->in + b->pos;
        int err = MP3Decode(dec, &ptr, &left, pcm, 0);
        b->pos = (size_t)(ptr - b->in);
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (!refill(b) && stream_done()) break;   // 末尾残缺帧
            continue;
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) continue;   // 比特池未满,正常
        if (err) {
            if (b->pos < b->len) b->pos++;
            if (++errors > MAX_DECODE_ERRORS) { error = "解码失败"; break; }
            continue;
        }
        errors = 0;
        if (!locked) {
            lock = hdr;
            locked = true;
        }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.nChans < 1 || fi.nChans > 2 || fi.samprate <= 0) continue;
        if ((uint32_t)fi.samprate != rate) {
            if (!open_codec((uint32_t)fi.samprate)) { error = "音频设备异常"; break; }
            rate = (uint32_t)fi.samprate;
            portENTER_CRITICAL(&s_mux);
            s_info.rate = rate;
            s_info.channels = (uint16_t)fi.nChans;
            portEXIT_CRITICAL(&s_mux);
        }
        size_t frames = (size_t)fi.outputSamps / (size_t)fi.nChans;
        if (fi.nChans == 2) bp_downmix_s16(pcm, frames);
        publish_level(pcm, frames);
        if (bsp_audio_write(pcm, frames * sizeof(int16_t)) != ESP_OK) {
            error = "音频设备异常";
            break;
        }
        frames_out += frames;
        uint32_t total = 0;
        if (fi.bitrate > 0) {
            bitrate_sum += (uint64_t)fi.bitrate;
            bitrate_frames++;
            uint32_t avg = (uint32_t)(bitrate_sum / bitrate_frames);
            if (file_size > id3) total = bp_estimate_ms(file_size - id3, avg);
            portENTER_CRITICAL(&s_mux);
            s_info.kbps = avg / 1000;
            portEXIT_CRITICAL(&s_mux);
        }
        publish_progress(frames_out, rate, total);
        if ((frames_out & 0x3fff) < frames) update_buffer_pct();
    }
    if (!error && !s_abort && frames_out == 0) error = "不是有效的 MP3";
out:
    return error;   // 解码器跨曲目持久化,不释放
}

// known 非空表示续播:流从 start(已按采样帧对齐)开始,没有文件头。
static const char *play_wav(inbuf_t *b, uint64_t file_size, const bp_wav_info_t *known,
                            uint64_t start) {
    bp_wav_info_t wav;
    if (known) {
        wav = *known;
    } else {
        int rc;
        while ((rc = bp_wav_parse(b->in, b->len, &wav)) == 1) {
            if (b->len >= IN_BUF_SIZE) return "WAV 头过大";
            if (!refill(b) && stream_done()) return "WAV 文件不完整";
        }
        if (rc != 0) return rc == -2 ? "仅支持 16 位 PCM WAV" : "不是有效的 WAV";
    }
    s_cur_wav = wav;
    s_cur_kind = BP_SNIFF_WAV;
    if (!open_codec(wav.rate)) return "音频设备异常";
    uint32_t align = (uint32_t)wav.channels * 2;
    // 流式录制的 WAV 常把 data 长度写成 0 或 0xFFFFFFFF,以文件大小为准。
    uint64_t avail = file_size > wav.data_offset ? file_size - wav.data_offset : 0;
    uint64_t data_bytes = wav.data_bytes;
    if (!data_bytes || data_bytes == 0xffffffffu || (avail && data_bytes > avail))
        data_bytes = avail ? avail : UINT64_MAX;
    uint32_t total_ms = data_bytes == UINT64_MAX ? 0 : bp_pcm_ms(data_bytes / align, wav.rate);
    portENTER_CRITICAL(&s_mux);
    s_info.rate = wav.rate;
    s_info.channels = wav.channels;
    s_info.kbps = wav.rate * align * 8 / 1000;
    s_info.total_ms = total_ms;
    portEXIT_CRITICAL(&s_mux);
    set_state(BP_PLAY_PLAYING, NULL);
    uint64_t played = 0, frames_out = 0;
    if (known) {
        played = start > wav.data_offset ? start - wav.data_offset : 0;
    } else {
        b->pos = wav.data_offset;
    }
    while (!s_abort && played < data_bytes) {
        wait_while_paused();
        size_t have = b->len - b->pos;
        if (have < align) {
            if (!refill(b) && stream_done()) break;
            continue;
        }
        size_t usable = have - have % align;
        if (usable > data_bytes - played) usable = (size_t)(data_bytes - played);
        int16_t *pcm = (int16_t *)(b->in + b->pos);
        size_t frames = usable / align;
        // in 缓冲起点 4 字节对齐,pos 按 align 递进;data_offset 为偶数,满足 int16 对齐。
        if (wav.channels == 2) bp_downmix_s16(pcm, frames);
        publish_level(pcm, frames);
        if (bsp_audio_write(pcm, frames * sizeof(int16_t)) != ESP_OK) return "音频设备异常";
        b->pos += usable;
        played += usable;
        frames_out += frames;
        publish_progress(frames_out, wav.rate, 0);
        if ((frames_out & 0x3fff) < frames) update_buffer_pct();
    }
    return NULL;
}

// ---- AAC(M4A 与 ADTS) -----------------------------------------------------------
// 每次解码前至少缓冲这么多字节:大于双声道 AAC-LC 单块上限(2 x 768 字节),
// 保证解码器不会读到未到达的数据(流末尾除外)。
#define AAC_MIN_AVAIL 2048

static void set_aac_info(uint32_t rate, uint16_t ch, uint32_t kbps, uint32_t total_ms) {
    portENTER_CRITICAL(&s_mux);
    s_info.rate = rate;
    s_info.channels = ch;
    s_info.kbps = kbps;
    if (total_ms) s_info.total_ms = total_ms;
    portEXIT_CRITICAL(&s_mux);
}

// 解码一帧后的公共处理:下混、写 I2S、发布进度。
static const char *aac_output(HAACDecoder dec, int16_t *pcm, uint64_t *frames_out,
                              uint32_t *rate, uint32_t total_ms) {
    AACFrameInfo fi;
    AACGetLastFrameInfo(dec, &fi);
    if (fi.nChans < 1 || fi.nChans > 2 || fi.sampRateOut <= 0 || fi.outputSamps <= 0) return NULL;
    if ((uint32_t)fi.sampRateOut != *rate) {
        if (!open_codec((uint32_t)fi.sampRateOut)) return "音频设备异常";
        *rate = (uint32_t)fi.sampRateOut;
    }
    size_t frames = (size_t)fi.outputSamps / (size_t)fi.nChans;
    if (fi.nChans == 2) bp_downmix_s16(pcm, frames);
    publish_level(pcm, frames);
    if (bsp_audio_write(pcm, frames * sizeof(int16_t)) != ESP_OK) return "音频设备异常";
    *frames_out += frames;
    publish_progress(*frames_out, *rate, total_ms);
    if ((*frames_out & 0x3fff) < frames) update_buffer_pct();
    return NULL;
}

// 从 MP4 的 mdat 中顺序解码裸 AAC 块(data_pos 为 b->pos 处的文件偏移)。
static const char *play_aac_raw(inbuf_t *b, const bp_m4a_info_t *m4a, uint64_t data_pos,
                                uint32_t total_ms) {
    s_cur_kind = BP_SNIFF_MP4;
    s_cur_m4a = *m4a;
    HAACDecoder dec = AACInitDecoder();
    log_heap("aac-decoder-ready");
    if (!dec) return "内存不足";
    const char *error = NULL;
    AACFrameInfo cfg = {.nChans = m4a->channels, .sampRateCore = (int)m4a->samplerate,
                        .profile = AAC_PROFILE_LC};
    if (AACSetRawBlockParams(dec, 0, &cfg) != 0) {
        AACFreeDecoder(dec);
        return "不支持的 AAC 参数";
    }
    uint64_t span = m4a->data_end > m4a->data_start ? m4a->data_end - m4a->data_start : 0;
    uint32_t kbps = total_ms ? (uint32_t)(span * 8 / total_ms) : 0;
    set_aac_info(m4a->samplerate, m4a->channels, kbps, total_ms);
    uint64_t remaining = m4a->data_end > data_pos ? m4a->data_end - data_pos : 0;
    uint64_t frames_out = 0;
    uint32_t rate = 0;
    int errors = 0;
    while (!s_abort && remaining > 0) {
        wait_while_paused();
        if (s_abort) break;
        if (b->len - b->pos < AAC_MIN_AVAIL && !stream_done()) refill(b);
        size_t avail = b->len - b->pos;
        int left = (int)(avail < remaining ? avail : remaining);
        if (left <= 0) {
            if (stream_done()) break;
            continue;
        }
        unsigned char *ptr = b->in + b->pos;
        int before = left;
        int err = AACDecode(dec, &ptr, &left, s_pcm_storage);
        size_t used = (size_t)(before - left);
        b->pos += used;
        remaining -= used;
        if (err == ERR_AAC_INDATA_UNDERFLOW) {
            if (!refill(b) && stream_done()) break;   // 文件被截断
            continue;
        }
        if (err) {
            // 裸 AAC 没有同步字,出错后无法重新对齐;连续失败即放弃。
            if (++errors > 8 || !used) {
                error = frames_out ? "AAC 解码失败" : "不支持的 AAC 编码";
                break;
            }
            continue;
        }
        errors = 0;
        if ((error = aac_output(dec, s_pcm_storage, &frames_out, &rate, total_ms))) break;
    }
    if (!error && !s_abort && frames_out == 0) error = "不支持的 AAC 编码";
    AACFreeDecoder(dec);
    return error;
}

// 输入缓冲上的顺序读取接口,供 M4A 头部解析使用。
static int inbuf_read(void *ctx, uint8_t *dst, size_t n) {
    inbuf_t *b = ctx;
    while (n) {
        if (s_abort) return -1;
        size_t have = b->len - b->pos;
        if (!have) {
            if (!refill(b) && stream_done()) return -1;
            continue;
        }
        size_t c = have < n ? have : n;
        memcpy(dst, b->in + b->pos, c);
        b->pos += c;
        dst += c;
        n -= c;
    }
    return 0;
}

static int inbuf_skip(void *ctx, uint64_t n) {
    inbuf_t *b = ctx;
    while (n) {
        if (s_abort) return -1;
        size_t have = b->len - b->pos;
        if (!have) {
            if (!refill(b) && stream_done()) return -1;
            continue;
        }
        size_t c = have < n ? have : (size_t)n;
        b->pos += c;
        n -= c;
    }
    return 0;
}

static const char *play_m4a(inbuf_t *b, uint64_t file_size) {
    bp_reader_t r = {inbuf_read, inbuf_skip, b, 0};
    bp_m4a_info_t info;
    int rc = bp_m4a_parse(&r, file_size, &info);
    if (s_abort) return NULL;
    ESP_LOGI(TAG, "m4a rc=%d rate=%lu ch=%u sbr=%d data=%llu..%llu", rc,
             (unsigned long)info.samplerate, info.channels, info.sbr,
             (unsigned long long)info.data_start, (unsigned long long)info.data_end);
    switch (rc) {
        case BP_M4A_OK: break;
        case BP_M4A_NO_AAC: return "不支持的 M4A 编码";
        case BP_M4A_MOOV_LAST: return "M4A 索引在文件末尾,暂不支持";
        case BP_M4A_IO: return s_fetch_failed ? "网络错误" : "M4A 文件不完整";
        default: return "不是有效的 M4A";
    }
    return play_aac_raw(b, &info, r.pos, bp_m4a_duration_ms(&info));
}

// ADTS:每帧自带同步字和参数,可从任意位置重新同步(续播也直接用)。
static const char *play_adts(inbuf_t *b, uint64_t file_size) {
    s_cur_kind = BP_SNIFF_AAC;
    HAACDecoder dec = AACInitDecoder();
    log_heap("aac-decoder-ready");
    if (!dec) return "内存不足";
    const char *error = NULL;
    uint64_t frames_out = 0;
    uint32_t rate = 0, total_ms = 0;
    size_t skipped = 0;
    int errors = 0;
    while (!s_abort) {
        wait_while_paused();
        if (s_abort) break;
        if (b->len - b->pos < AAC_MIN_AVAIL && !stream_done()) refill(b);
        int left = (int)(b->len - b->pos);
        if (left <= 0) {
            if (stream_done()) break;
            continue;
        }
        int sync = AACFindSyncWord(b->in + b->pos, left);
        if (sync < 0) {
            size_t drop = left > 1 ? (size_t)left - 1 : (size_t)left;
            b->pos += drop;
            if ((skipped += drop) > 256 * 1024) { error = "不是有效的 AAC"; break; }
            if (!refill(b) && stream_done()) break;
            continue;
        }
        b->pos += (size_t)sync;
        left -= sync;
        unsigned char *ptr = b->in + b->pos;
        int before = left;
        int err = AACDecode(dec, &ptr, &left, s_pcm_storage);
        b->pos += (size_t)(before - left);
        if (err == ERR_AAC_INDATA_UNDERFLOW) {
            if (!refill(b) && stream_done()) break;
            continue;
        }
        if (err) {
            if (b->pos < b->len) b->pos++;   // 跳过这个同步字,继续找下一帧
            if (++errors > MAX_DECODE_ERRORS) { error = "AAC 解码失败"; break; }
            continue;
        }
        errors = 0;
        skipped = 0;
        if (!total_ms) {
            AACFrameInfo fi;
            AACGetLastFrameInfo(dec, &fi);
            if (fi.bitRate > 0) total_ms = bp_estimate_ms(file_size, (uint32_t)fi.bitRate);
            set_aac_info((uint32_t)fi.sampRateOut, (uint16_t)fi.nChans,
                         (uint32_t)fi.bitRate / 1000, total_ms);
        }
        if ((error = aac_output(dec, s_pcm_storage, &frames_out, &rate, total_ms))) break;
    }
    if (!error && !s_abort && frames_out == 0) error = "不是有效的 AAC";
    AACFreeDecoder(dec);
    return error;
}

static void decode_task(void *arg) {
    track_t *t = arg;
    const char *error = NULL;
    inbuf_t b = {.in = s_in_storage};   // 4 字节对齐(WAV 按 int16 读)
    s_fetch_eof = s_fetch_failed = s_fetch_parked = false;
    s_cur_kind = BP_SNIFF_MP3;
    s_base_ms = t->resume ? s_resume.pos_ms : 0;
    s_fetch_running = true;
    if (xTaskCreate(fetch_task, "bp_fetch", FETCH_STACK, t, 5, NULL) != pdPASS) {
        s_fetch_running = false;
        error = "内存不足";
        goto done;
    }
    // 初始缓冲 75%(≈10.5KB=0.26s@320kbps)再开播:比原 50% 多等 0.13s,
    // 但开播后缓冲水位更高,前几秒更不容易再次进入"缓冲中"。
    if (!wait_buffered(75)) goto done;
    refill(&b);
    if (b.len < 4) {
        error = s_fetch_failed ? "网络错误" : "文件为空";
        goto done;
    }
    if (t->resume) {
        if (s_resume.kind == BP_SNIFF_WAV) {
            error = play_wav(&b, t->size, &s_resume.wav, t->start);
        } else if (s_resume.kind == BP_SNIFF_MP4) {
            error = play_aac_raw(&b, &s_resume.m4a, t->start, s_resume.total_ms);
        } else if (s_resume.kind == BP_SNIFF_AAC) {
            error = play_adts(&b, t->size);
        } else {
            error = play_mp3(&b, t->size, 0);   // 重新同步帧头
        }
    } else {
        // 按内容识别格式:网盘里常有 M4A 等文件被命名为 .mp3,硬当 MP3 解会误判帧头而卡顿。
        bp_sniff_t kind = bp_media_sniff(b.in, b.len);
        ESP_LOGI(TAG, "content sniff=%d", kind);
        static const char *const FMT[] = {NULL, "MP3", "WAV", "M4A", "AAC", "FLAC", "OGG"};
        portENTER_CRITICAL(&s_mux);
        s_info.fmt = (size_t)kind < sizeof(FMT) / sizeof(FMT[0]) ? FMT[kind] : NULL;
        portEXIT_CRITICAL(&s_mux);
        if (kind == BP_SNIFF_WAV) {
            error = play_wav(&b, t->size, NULL, 0);
        } else if (kind == BP_SNIFF_MP4) {
            error = play_m4a(&b, t->size);
        } else if (kind == BP_SNIFF_AAC) {
            error = play_adts(&b, t->size);
        } else if (kind == BP_SNIFF_FLAC) {
            error = "FLAC 格式暂不支持";
        } else if (kind == BP_SNIFF_OGG) {
            error = "OGG 格式暂不支持";
        } else {
            size_t id3 = bp_id3v2_size(b.in, b.len);
            if (id3 && !discard(&b, id3)) goto done;
            if (!s_abort) error = play_mp3(&b, t->size, id3);
        }
    }
    if (!error && s_fetch_failed && !s_abort) error = "网络中断";
done:
    s_abort = true;   // 通知 fetch 退出(若尚在运行)
    while (s_fetch_running) vTaskDelay(pdMS_TO_TICKS(20));
    log_heap("pipeline-end");
    // 流水线拆除(曲目结束/挂起/停止)时不恢复 arena:保持释放让列表请求可用。
    // 播放器真正进入 idle(列表播完/停止)时由 finish_idle 里的 restore("idle") 预留。
    // 连续切歌时下一首 start_track 的 release() 会走 probe 快速重拿(通常立即成功)。
    // 记录挂起点:下一个未解码字节 = 起始偏移 + 已读入 - 输入缓冲中未消费的部分。
    portENTER_CRITICAL(&s_mux);
    s_resume.offset = t->start + b.received - (b.len - b.pos);
    s_resume.pos_ms = s_info.pos_ms;
    s_resume.total_ms = s_info.total_ms;
    s_resume.kind = s_cur_kind;
    s_resume.wav = s_cur_wav;
    s_resume.m4a = s_cur_m4a;
    portEXIT_CRITICAL(&s_mux);
    uint32_t gen = t->gen;
    bool aborted_by_user;
    portENTER_CRITICAL(&s_mux);
    aborted_by_user = s_gen != gen;
    portEXIT_CRITICAL(&s_mux);
    free(t->url);
    free(t);
    s_pipeline_running = false;
    if (!aborted_by_user) post(error ? CMD_TRACK_FAIL : CMD_TRACK_END, gen, error);
    vTaskDelete(NULL);
}

// ---- ctrl 任务 ------------------------------------------------------------------
static void stop_pipeline(void) {
    if (!s_pipeline_running) return;
    portENTER_CRITICAL(&s_mux);
    s_gen++;              // 使流水线结束事件过期
    portEXIT_CRITICAL(&s_mux);
    s_abort = true;
    while (s_pipeline_running) vTaskDelay(pdMS_TO_TICKS(20));
}

// ---- 记住上次播放的位置(目录/全部音频的那一页 + 曲目),开机后从这里接着播 ----------
#define LAST_NS "bp_last"

typedef struct {
    uint8_t source;        // bp_source_t
    int32_t page;
    char marker[BP_MARKER_MAX];   // 该页在网盘接口里的游标
    char file_id[BP_FILE_ID_MAX]; // 当前曲目
    char parent_file_id[BP_FILE_ID_MAX]; // 该页所在目录的 file_id
    char dir[BP_PATH_MAX];
} last_play_t;

static last_play_t s_last_saved;

static void remember_last(const bp_file_t *f) {
    last_play_t lp;
    memset(&lp, 0, sizeof(lp));   // 含填充字节,便于 memcmp
    lp.source = (uint8_t)s_pl_req.source;
    lp.page = s_pl_req.page;
    strlcpy(lp.marker, s_pl_req.marker, sizeof(lp.marker));
    strlcpy(lp.file_id, f->file_id, sizeof(lp.file_id));
    strlcpy(lp.parent_file_id, s_pl_req.parent_file_id, sizeof(lp.parent_file_id));
    strlcpy(lp.dir, s_pl_req.dir, sizeof(lp.dir));
    if (memcmp(&lp, &s_last_saved, sizeof(lp)) == 0) return;   // 未变化不重复写 flash
    nvs_handle_t h;
    if (nvs_open(LAST_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "pos", &lp, sizeof(lp)) == ESP_OK && nvs_commit(h) == ESP_OK) s_last_saved = lp;
    nvs_close(h);
}

static bool load_last(last_play_t *lp) {
    nvs_handle_t h;
    if (nvs_open(LAST_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*lp);
    memset(lp, 0, sizeof(*lp));
    bool ok = nvs_get_blob(h, "pos", lp, &len) == ESP_OK && len == sizeof(*lp);
    nvs_close(h);
    lp->dir[sizeof(lp->dir) - 1] = '\0';
    return ok && lp->source <= BP_SRC_ALL_AUDIO;
}

static void start_track_at(int index, bool resume) {
    stop_pipeline();
    s_suspended = false;
    if (index < 0 || index >= s_pl_count) {
        set_state(BP_PLAY_IDLE, NULL);
        return;
    }
    // 还没播出声音就挂起的,从头重播即可(也避开落在 ID3 标签里的偏移)。
    if (resume && s_resume.pos_ms == 0) resume = false;
    uint64_t start = 0;
    if (resume) {
        start = s_resume.offset;
        if (s_resume.kind == BP_SNIFF_WAV) {   // 对齐到采样帧边界
            uint32_t align = (uint32_t)s_resume.wav.channels * 2;
            uint64_t off = s_resume.wav.data_offset;
            start = start > off ? off + (start - off) / align * align : off;
        }
    }
    const bp_file_t *f = &s_playlist[index];
    if (!resume) remember_last(f);
    uint32_t gen;
    portENTER_CRITICAL(&s_mux);
    gen = ++s_gen;
    s_pl_index = index;
    if (!resume) {
        memset(&s_info, 0, sizeof(s_info));
        strlcpy(s_info.name, f->name, sizeof(s_info.name));
        strlcpy(s_info.folder, f->folder, sizeof(s_info.folder));
        s_info.index = index;
        s_info.count = s_pl_count;
    }
    s_info.state = BP_PLAY_RESOLVING;
    s_info.error = NULL;
    portEXIT_CRITICAL(&s_mux);
    s_paused = false;

    char *url = NULL;
    heap_arena_release("play");   // 播放全程(含 dlink)把预留块让给 TLS 大记录分配
    mp3_ensure();                 // 堆最平整、无 TLS 会话的窗口:初始化持久化解码器
    log_heap("before-dlink");
    if (bp_aliyun_dlink(f->file_id, &url) != 0) {
        set_state(BP_PLAY_ERROR, g_bp.wifi_up ? "获取下载地址失败" : "无网络");
        post(CMD_TRACK_FAIL, gen, NULL);   // 让 ctrl 统一处理跳过逻辑
        return;
    }
    track_t *t = calloc(1, sizeof(track_t));
    xStreamBufferReset(s_stream);   // 上一条流水线已退出,缓冲可安全清空
    if (!t) {
        free(url);
        set_state(BP_PLAY_ERROR, "内存不足");
        post(CMD_TRACK_FAIL, gen, NULL);
        return;
    }
    t->url = url;
    t->size = f->size;
    t->gen = gen;
    t->start = start;
    t->resume = resume;
    s_abort = false;
    s_pipeline_running = true;
    ESP_LOGI(TAG, "play #%d gen=%lu stream=%uKB free=%u largest=%u", index,
             (unsigned long)gen, (unsigned)(s_stream_size / 1024),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (resume) ESP_LOGI(TAG, "resume at byte %" PRIu64 " (%lu ms)", start,
                         (unsigned long)s_resume.pos_ms);
    esp_wifi_set_ps(WIFI_PS_NONE);
    set_state(BP_PLAY_BUFFERING, NULL);
    if (xTaskCreate(decode_task, "bp_decode", DECODE_STACK, t, 6, NULL) != pdPASS) {
        s_pipeline_running = false;
        free(t->url);
        free(t);
        set_state(BP_PLAY_ERROR, "内存不足");
        post(CMD_TRACK_FAIL, gen, NULL);
    }
}

static void start_track(int index) { start_track_at(index, false); }

// 列表播完:有下一页就加载并继续,否则停止。返回 true 表示已开始新曲目。
static bool continue_next_page(void) {
    if (!s_pl_has_more) return false;
    bp_list_t *page = malloc(sizeof(bp_list_t));
    if (!page) return false;
    bp_list_req_t req = s_pl_req;
    req.page++;
    strlcpy(req.marker, s_pl_next_marker, sizeof(req.marker));
    bool started = false;
    set_state(BP_PLAY_RESOLVING, NULL);
    if (bp_aliyun_list_fetch(&req, page) == 0) {
        int n = 0;
        for (int i = 0; i < page->count; i++)
            if (!page->files[i].is_dir && page->files[i].format != BP_FMT_UNKNOWN)
                s_playlist[n++] = page->files[i];
        s_pl_req = req;
        s_pl_has_more = page->has_more;
        strlcpy(s_pl_next_marker, page->next_marker, sizeof(s_pl_next_marker));
        if (n) {
            s_pl_count = n;
            start_track(0);
            started = true;
        } else if (s_pl_has_more) {
            started = continue_next_page();   // 整页无可播放文件,继续翻
        }
    }
    free(page);
    return started;
}

bool bp_player_last_label(char *out, size_t cap) {
    last_play_t lp;
    if (!load_last(&lp)) return false;
    if (!out || !cap) return true;
    const char *slash = strrchr(lp.dir, '/');
    const char *name = lp.source == BP_SRC_ALL_AUDIO ? "全部音频" :
                       slash && slash[1] ? slash + 1 : "我的网盘";
    bp_name_shorten(name, out, cap);   // 按 UTF-8 字符缩短
    return true;
}

// 开机续播:取回记住的那一页,从记住的曲目开始(找不到就从该页第一首开始)。
// 返回 1 已开始,0 没有记录,-1 有记录但载入失败。
static int play_last(void) {
    last_play_t lp;
    if (!load_last(&lp)) return 0;
    s_last_saved = lp;
    bp_list_t *page = malloc(sizeof(bp_list_t));
    if (!page) return -1;
    bp_list_req_t req = {.source = (bp_source_t)lp.source, .page = lp.page};
    strlcpy(req.marker, lp.marker, sizeof(req.marker));
    strlcpy(req.parent_file_id, lp.parent_file_id, sizeof(req.parent_file_id));
    strlcpy(req.dir, lp.dir, sizeof(req.dir));
    bool started = false;
    if (bp_aliyun_list_fetch(&req, page) == 0) {
        int n = 0, index = 0;
        for (int i = 0; i < page->count; i++) {
            const bp_file_t *f = &page->files[i];
            if (f->is_dir || f->format == BP_FMT_UNKNOWN) continue;
            if (strcmp(f->file_id, lp.file_id) == 0) index = n;
            s_playlist[n++] = *f;
        }
        if (n) {
            s_pl_count = n;
            s_pl_req = req;
            s_pl_has_more = page->has_more;
            strlcpy(s_pl_next_marker, page->next_marker, sizeof(s_pl_next_marker));
            start_track(index);
            started = true;
        }
    }
    free(page);
    ESP_LOGI(TAG, "resume last folder %s: %s", lp.dir, started ? "playing" : "unavailable");
    return started ? 1 : -1;
}

static void finish_idle(void) {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    heap_arena_restore("idle");   // dlink 失败等不走流水线收尾的路径也要归还预留块
    portENTER_CRITICAL(&s_mux);
    s_info.pos_ms = 0;   // 停止后进度归零;曲名保留,单击 OK 可重播
    s_info.buffer_pct = 0;
    portEXIT_CRITICAL(&s_mux);
    set_state(BP_PLAY_IDLE, NULL);
}

static void ctrl_task(void *arg) {
    (void)arg;
    int consecutive_failures = 0;
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmd, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.type) {
            case CMD_PLAY: {
                portENTER_CRITICAL(&s_mux);
                memcpy(s_playlist, s_pending_list, sizeof(s_playlist));
                s_pl_count = s_pending_count;
                s_pl_req = s_pending_req;
                s_pl_has_more = s_pending_has_more;
                strlcpy(s_pl_next_marker, s_pending_next_marker, sizeof(s_pl_next_marker));
                int index = s_pending_index;
                portEXIT_CRITICAL(&s_mux);
                consecutive_failures = 0;
                start_track(index);
                break;
            }
            case CMD_NEXT: {
                int next = bp_playlist_move(s_pl_index, s_pl_count, 1, false);
                consecutive_failures = 0;
                if (next >= 0) {
                    start_track(next);
                    break;
                }
                stop_pipeline();
                if (!continue_next_page()) start_track(0);   // 没有更多页:回到列表开头
                break;
            }
            case CMD_PREV: {
                bp_player_info_t info;
                bp_player_get_info(&info);
                consecutive_failures = 0;
                if (info.pos_ms > 3000 && info.state != BP_PLAY_ERROR) start_track(s_pl_index);
                else start_track(bp_playlist_move(s_pl_index, s_pl_count, -1, true));
                break;
            }
            case CMD_PLAY_LAST:
                consecutive_failures = 0;
                if (!s_pipeline_running) {
                    int r = play_last();
                    if (r < 0) set_state(BP_PLAY_ERROR, "上次的目录暂时无法载入");
                    else if (r == 0) finish_idle();
                }
                break;
            case CMD_STOP:
                stop_pipeline();
                s_suspended = false;
                finish_idle();
                break;
            case CMD_SUSPEND:
                if (s_pipeline_running && !s_suspended) {
                    s_paused = true;
                    stop_pipeline();   // decode 任务退出时记录挂起点
                    s_suspended = true;
                    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                    set_state(BP_PLAY_PAUSED, NULL);
                    log_heap("suspended");
                }
                break;
            case CMD_RESUME:
                if (s_suspended) start_track_at(s_pl_index, true);
                break;
            case CMD_REPLAY:
                consecutive_failures = 0;
                start_track(s_pl_index);
                break;
            case CMD_TRACK_END:
            case CMD_TRACK_FAIL: {
                if (c.gen != s_gen) break;   // 已被新命令取代
                if (c.type == CMD_TRACK_FAIL) {
                    if (c.error) set_state(BP_PLAY_ERROR, c.error);
                    ESP_LOGW(TAG, "track failed: %s", s_info.error ? s_info.error : "?");
                    // 停留 2 秒让用户看到原因;连续失败 3 首就停下,避免空转。
                    if (++consecutive_failures >= 3) {
                        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    if (c.gen != s_gen || uxQueueMessagesWaiting(s_cmd)) break;
                } else {
                    consecutive_failures = 0;
                }
                int next = bp_playlist_move(s_pl_index, s_pl_count, 1, false);
                if (next >= 0) start_track(next);
                else if (!continue_next_page()) finish_idle();
                break;
            }
        }
    }
}

// ---- 公共接口 -------------------------------------------------------------------
int bp_player_play_list(const bp_list_t *list, int index) {
    if (!list || index < 0 || index >= list->count) return -1;
    int n = 0, start = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < list->count && n < BP_PLAYLIST_MAX; i++) {
        const bp_file_t *f = &list->files[i];
        if (f->is_dir || f->format == BP_FMT_UNKNOWN) continue;
        if (i == index) start = n;
        s_pending_list[n++] = *f;
    }
    s_pending_count = n;
    s_pending_index = start;
    s_pending_req = list->req;
    s_pending_has_more = list->has_more;
    strlcpy(s_pending_next_marker, list->next_marker, sizeof(s_pending_next_marker));
    portEXIT_CRITICAL(&s_mux);
    if (start < 0) return -2;
    post(CMD_PLAY, 0, NULL);
    return 0;
}

void bp_player_toggle_pause(void) {
    if (s_suspended) {
        post(CMD_RESUME, 0, NULL);
        return;
    }
    bp_play_state_t st = s_info.state;
    if (st == BP_PLAY_PLAYING || st == BP_PLAY_BUFFERING || st == BP_PLAY_PAUSED)
        s_paused = !s_paused;
    else if ((st == BP_PLAY_IDLE || st == BP_PLAY_ERROR) && s_pl_count > 0)
        post(CMD_REPLAY, 0, NULL);
}

bool bp_player_suspend(void) {
    bp_play_state_t st = s_info.state;
    if (s_suspended || !s_pipeline_running ||
        (st != BP_PLAY_PLAYING && st != BP_PLAY_BUFFERING && st != BP_PLAY_PAUSED))
        return false;
    bool was_playing = !s_paused;
    s_paused = true;   // 立即静音,拆除由 ctrl 任务完成
    post(CMD_SUSPEND, 0, NULL);
    return was_playing;
}

void bp_player_wait_released(int timeout_ms) {
    for (int waited = 0; waited < timeout_ms && s_pipeline_running; waited += 50)
        vTaskDelay(pdMS_TO_TICKS(50));
}

void bp_player_next(void) { post(CMD_NEXT, 0, NULL); }
void bp_player_prev(void) { post(CMD_PREV, 0, NULL); }
void bp_player_stop(void) { post(CMD_STOP, 0, NULL); }
void bp_player_play_last(void) { post(CMD_PLAY_LAST, 0, NULL); }

void bp_player_set_volume(uint8_t volume) {
    g_bp.volume = volume > 100 ? 100 : volume;
    bsp_audio_set_volume(g_bp.volume);
    nvs_handle_t h;
    if (nvs_open("bp_player", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", g_bp.volume);
        nvs_commit(h);
        nvs_close(h);
    }
}

void bp_player_get_info(bp_player_info_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

bool bp_player_active(void) {
    bp_play_state_t st = s_info.state;
    return st == BP_PLAY_RESOLVING || st == BP_PLAY_BUFFERING || st == BP_PLAY_PLAYING ||
           st == BP_PLAY_PAUSED;
}

void bp_player_init(void) {
    uint8_t vol = 60;
    nvs_handle_t h;
    if (nvs_open("bp_player", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "vol", &vol);
        nvs_close(h);
    }
    g_bp.volume = vol > 100 ? 60 : vol;
    if (bsp_audio_init() != ESP_OK) ESP_LOGE(TAG, "audio init failed");
    bsp_audio_set_volume(g_bp.volume);
    s_stream = xStreamBufferCreateStatic(STREAM_SIZE, 1, s_stream_storage, &s_stream_struct);
    s_cmd = xQueueCreate(6, sizeof(cmd_t));
    if (!s_cmd || xTaskCreate(ctrl_task, "bp_ctrl", CTRL_STACK, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "player task create failed");
    // 竞技场改为惰性预留:开机就占用会挤死启动期首次 TLS 验签(实测 min=196)。
    // 第一次播放收尾/暂停/停止时由 heap_arena_restore 建立。
}
