// main/bp_keys.h —— 遗留的百度应用凭据运行时解码(阿里云盘后端不使用,保留以兼容上游构建)。
//
// 凭据在编译时由 tools/obfuscate_keys.py 编码成两组字节数组,固件里没有明文,
// 用 strings / 十六进制搜索扫不出来。需要时解码到调用方的临时缓冲区,用完请
// bp_key_wipe() 清零。这只防静态扫描,不防逆向。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define BP_KEY_BUF 80   // 足够容纳 AppKey / SecretKey 与结尾 NUL

typedef enum {
    BP_KEY_APPKEY = 0,
    BP_KEY_SECRET,
} bp_key_id_t;

// 解码到 out(以 NUL 结尾),返回长度;cap 不足返回 0 且 out 为空串。
size_t bp_key_get(bp_key_id_t id, char *out, size_t cap);
// 清零缓冲区(不会被编译器优化掉)。
void bp_key_wipe(void *buf, size_t len);
// 是否为未配置的占位符凭据。
bool bp_keys_placeholder(void);
