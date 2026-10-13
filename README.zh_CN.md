<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

<h1 align="center">阿里云盘随身听</h1>

<p align="center">
  <strong>把阿里云盘里的音乐，装进一枚随身工牌。</strong><br>
  运行在 FoloToy AI Passport(ESP32-C3,无 PSRAM)上的 MP3/M4A/AAC/WAV 流式播放固件。
</p>

---

## 功能

- **直接播放网盘音频**:不占本地存储,不需要手机 App。可按文件夹浏览,也可按名称顺序列出网盘内全部 MP3、M4A、AAC 和 WAV。
- **扫码登录**:屏幕显示二维码,用手机阿里云盘 App 扫码确认即可;授权保存在设备上并自动续期。
- **网页配网**:首次开机自动开启热点 `AliyunPlayer-XXXX`,扫屏幕二维码连上后在弹出的网页里选网络、填密码。
- **连续播放**:自动播放下一首并加载下一页;下载中断后按 HTTP Range 断点续传;音量重启后保留。
- **中文文件名**:内置字库覆盖 GB2312 全部 7445 个字符。
- **三键操作**:专为 240×320 圆角屏设计;30 秒无操作自动熄屏,音乐继续播放。

## 快速开始

1. **编译烧录**(ESP-IDF 5.5.3):

   ```bash
   idf.py set-target esp32c3
   idf.py build
   idf.py -p <端口> flash          # 首次安装
   idf.py -p <端口> app-flash      # 之后更新,保留 Wi-Fi 与授权
   ```

   完整门禁 `./tools/validate.sh` 会运行仓库检查、主机测试并生成校验过的合并镜像。
2. **连接 Wi-Fi**:手机扫屏幕二维码加入热点 `AliyunPlayer-XXXX`,配网网页会自动弹出(或访问
   `http://192.168.4.1`)。仅支持 2.4 GHz。
3. **绑定网盘并播放**:联网后自动进入绑定页,用**阿里云盘 App** 扫屏幕二维码并确认授权;
   完成后自动打开“全部音频”,选中歌曲按 OK 即可播放。

> 无需任何应用凭据:阿里云盘登录使用设备屏幕上的官方 OAuth 扫码流程,固件中不内嵌任何密钥。

## 按键

| 页面 | 上 / 下 | OK | 长按 OK | 长按上 / 下 |
| --- | --- | --- | --- | --- |
| 播放界面(开机即进入) | 音量 ±10 | 播放 / 暂停(还没有曲目时打开“全部音频”) | 进入设置 | 上一首 / 下一首 |
| 设置菜单 | 选择 | 进入 | 回到播放界面 | — |
| 列表 | 选择 | 打开文件夹 / 播放 / 翻页 | 上一级 / 返回设置 | 快速移动 5 行 |
| 播放界面选择 | 经典 / 磁带 / 布鲁伊 / 小猪佩奇(2×2 卡片) | 应用(保存) | 返回设置 | — |
| 屏幕亮度 | 1～5 档(默认 3,保存) | — | 返回设置 | — |
| 无线网络 | — | 开启配网热点 | 返回设置 | — |

设置菜单包含:全部音频、浏览网盘、播放界面、屏幕亮度、无线网络、网盘账号、关于与按键、返回(回到播放页,同长按 OK)。
**磁带**界面有两个转动的磁带轮(带少的一侧转得快)、走带线上一黑一白两条跳动的声波,以及一条
切成两段、头尾轻轻摆动并用声波竖条连接的鱼;**经典**界面是进度环。**布鲁伊**与**小猪佩奇**
是磁带界面的变体,两个磁带轮换成转动的人物头像(蓝、橙两只小狗;粉色小猪),为非官方的同人绘制。

**开机续播**:会记住上次播放的目录(或“全部音频”)和曲目。下次开机时播放页先显示“正在连接网络…”、
联网后显示“正在载入上次播放…”和目录名,随后从那首曲目接着播;目录已不存在时会提示并停在播放页。

## 支持的格式

| 格式 | 说明 |
| --- | --- |
| MP3 | MPEG-1/2 Layer III,CBR 与 VBR,任意采样率(Helix 解码) |
| M4A | MP4 容器中的 AAC-LC(Helix AAC);HE-AAC 播放其 AAC-LC 基础层。索引需在文件开头 |
| AAC | ADTS 格式的 AAC-LC |
| WAV | 16 位 PCM,单/双声道,8–48 kHz |

板载单喇叭,双声道下混为单声道。浏览网盘时只显示文件夹和可播放的文件,FLAC、OGG 及其他文件不显示。格式按文件内容识别,扩展名写成 `.mp3` 的 M4A 也能播放。暂不支持拖动进度;
浏览网盘时如果正在播放会自动暂停(释放下载连接腾出内存),回到播放页按 OK 继续。

## 工作原理

```text
阿里云盘扫码登录 ─► /v2/account/token(refresh_token 轮换) ─► /adrive/v3/file/list ─► 下载直链
                                                                                          │
控制任务 ─► 下载任务(HTTPS、302 跳转、Range 续传) ─► 14 KB 环形缓冲
                                                                                          │
                        解码任务(Helix MP3 / AAC / WAV) ─► 单声道 PCM ─► ES8311 / I2S
```

| 模块 | 职责 |
| --- | --- |
| [`main/bp_aliyun.c`](main/bp_aliyun.c) | 扫码授权、refresh_token 轮换、目录与音频列表、下载直链 |
| [`main/bp_player.c`](main/bp_player.c) | 流式播放流水线、播放列表、自动续播、分阶段内存日志 |
| [`main/bp_ui.c`](main/bp_ui.c) | LVGL 页面:播放、列表、设置常驻;不常用的页面进入时创建、离开时释放 |
| [`main/bp_ui_reel.c`](main/bp_ui_reel.c) | 磁带界面:转轮与鱼的动画帧预渲染在 Flash,声波幅度跟随实时音量 |
| [`main/bp_wifi.cc`](main/bp_wifi.cc) | STA 自动回连与 SoftAP 强制门户配网 |
| [`main/bp_media.c`](main/bp_media.c) | 带主机测试的纯逻辑:格式、按内容识别、MP3 帧头校验、ID3v2、WAV 头、路径 |
| [`main/bp_mp4.c`](main/bp_mp4.c) | 流式 M4A 头部解析;之后把 mdat 当作连续的裸 AAC 块解码,无需逐帧大小表 |

ESP32-C3 全部可用堆约 180 KB。实测播放时:每首开始前空闲约 106 KB,CDN 的 TLS 连接建立后约
52 KB,解码中最低约 20 KB,每首结束后完全回收。详细说明、串口命令与限制见
[docs/aliyundrive-pocket-player.zh_CN.md](docs/aliyundrive-pocket-player.zh_CN.md)。

## 致谢

- 本仓库是 [lewangdev/baidupan-pocket-player](https://github.com/lewangdev/baidupan-pocket-player)(百度网盘随身听)的
  fork,后者基于 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport)(板级支持、构建与校验工具),
  其模板文档保留在 [docs/README.zh_CN.md](docs/README.zh_CN.md)。
- 阿里云盘接入(设备扫码登录、refresh_token 轮换、盘/文件接口)为本 fork 的原创工作,
  参考社区公开的阿里云盘 OAuth 流程实现。
- Wi-Fi 与配网门户来自 [esp-wifi-connect](https://github.com/78/esp-wifi-connect),AAC 解码为 Helix AAC(取自 [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio),RPSL),MP3 解码来自
  [libhelix-mp3](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3),中文字体为思源黑体(SIL OFL)。

以 [MIT 许可证](LICENSE)发布。