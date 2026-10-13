<p align="right">
  <a href="aliyundrive-pocket-player.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AliyunDrive Pocket Player

Streams MP3 / M4A / AAC / WAV audio stored in Aliyun Drive on the FoloToy AI Passport. The drive
integration (device QR-code authorization, refresh-token rotation, drive/file APIs) is original work
in this fork and follows the community-documented Aliyun Drive OAuth flow.

## Getting started

1. Wi-Fi (2.4 GHz only): with no saved network, the device starts the open hotspot `AliyunPlayer-XXXX`
   at boot and shows its QR code. Join it with a phone; the setup page opens (otherwise visit
   `http://192.168.4.1`). Pick a network and enter the password. The device saves it only after a
   successful test connection, then closes the hotspot and connects. Reopen the hotspot from
   Settings → Wi-Fi with OK. Several networks can be saved. The serial command
   `WIFI SET <ssid>|<password>` also works.
2. Binding: once Wi-Fi connects, the device opens the binding page by itself. Scan the QR code with
   the Aliyun Drive app and confirm; *All audio* opens automatically. The refresh token is stored on the
   device and rotated automatically; sign out under Settings → Netdisk account. If you leave the page
   with a long OK, bind later from Settings → Netdisk account.
3. Playback: "All audio" lists every MP3/M4A/AAC/WAV file in name order; "Browse" walks folders, also sorted by name.
   Browsing while a track plays pauses it and releases the download connection; press OK on the player to resume. Press OK on a
   track to play. The next track starts automatically, and the next page loads at the end of a page.

## Buttons

| Screen | Up / Down | OK | Long OK | Long Up / Down |
| --- | --- | --- | --- | --- |
| Player (start screen) | Volume ±10 | Play / pause (opens *All audio* if nothing is loaded) | Open Settings | Previous / next track |
| Settings menu | Select | Open | Back to the player | — |
| List | Select | Open folder / play / page | Parent folder / Settings | Move 5 rows |
| Player skin | Classic, Tape, Bluey or Peppa (2×2 grid) | Apply (saved) | Back to Settings | — |
| Brightness | Level 1–5 (default 3, saved) | — | Back to Settings | — |
| Wi-Fi | — | Start setup hotspot | Back to Settings | — |

The Settings menu holds *All audio*, *Browse*, *Player skin*, *Brightness*, *Wi-Fi*, *Netdisk account*,
*About* and *Back* (returns to the player, like a long OK). The **Tape** skin shows two spinning reels (the emptier reel spins faster), black and white
waveforms on the tape, and a fish cut in two whose swaying head and tail are joined by live
sound-wave bars; the **Classic** skin is the progress ring. The **Bluey** and **Peppa** skins are tape
variants whose reels are spinning fan-drawn character faces (a blue and an orange pup; a pink pig).

**Resume on boot.** The player remembers the folder (or *All audio*) and the track you last played. At the
next power-on it shows a “connecting to network” hint and then a “resuming last playback” hint with the
folder name on the player page, and starts that track again once Wi-Fi is up. If the folder is gone,
it says so and stays on the player.

On the setup hotspot page, a long OK closes the hotspot and goes back.

The screen turns off after 30 s without input while music keeps playing. A key press only wakes it.

## Formats and limits

- MP3 (MPEG-1/2 Layer III, CBR/VBR, any sample rate) and 16-bit PCM WAV (mono/stereo, 8–48 kHz).
  The single speaker receives a mono downmix.
- M4A (AAC-LC in MP4) and ADTS AAC. HE-AAC plays its AAC-LC core (fewer high frequencies). M4A files
  must keep their index (moov) before the audio data, which is the usual layout.
- The format is detected from file content, so an M4A file named `.mp3` plays correctly.
- FLAC, OGG and other formats are not supported. Browsing shows only folders and playable files;
  everything else is hidden.
- Seeking is not supported. VBR MP3 duration is estimated from the average bitrate.
- Interrupted downloads resume with HTTP Range, including after a long pause closes the connection.

## Implementation overview

| Module | Responsibility |
| --- | --- |
| `main/bp_aliyun.c` | Device QR-code auth, refresh-token rotation, folder and audio lists, download links |
| `main/bp_player.c` | Control task → fetch task (follows 302, Range resume) → static 14 KiB stream buffer → decode task (Helix MP3 / AAC / WAV) → I2S |
| `main/bp_ui.c` | Player, list and settings pages stay resident; other pages are built on entry and freed on exit; only the active skin exists |
| `main/bp_ui_reel.c` | Tape skin: 24 reel frames and 9 swing frames per fish half (plus 36-frame character discs) pre-rendered in flash (no runtime rotation), waveforms follow the live audio level |
| `main/bp_media.c` | Pure logic: format detection and content sniffing, MP3 frame checks, ID3v2 skip, WAV parsing, duration, paths (host test `tests/test_bp_media.c`) |
| `main/bp_mp4.c` | Streaming M4A header parser; mdat is decoded as consecutive raw AAC blocks (host test `tests/test_bp_mp4.c`) |
| `main/bp_console.c` | Serial commands: `WIFI SET/LIST/DEL/AP`, `ALI TOKEN/LOGOUT/DRIVE/QR/SHOW/SCAN`, `STATE`, `LIST`, `HEAP`, `STOP`, `KEY`, `UI` |

The `bp_font_16` Chinese font contains all GB2312 characters to render arbitrary file names; it is
generated by `tools/generate_bp_fonts.py`.