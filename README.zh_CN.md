[English](README.md) · [简体中文](README.zh_CN.md)

# Knot 结绳记事 · FoloToy AI Passport

按一下 [FoloToy AI Passport](https://ai-passport.folotoy.cn/) 的确定键，这一秒就存进设备，通过蓝牙交给电脑，
插进 Obsidian 当天日记，留给你之后补写：

```markdown
## Log
- 10:40 到达工位
- 2026-11-17 10:53:54 
- 2026-11-17 11:49:03 #START 
- 2026-11-17 15:50:50 🎙 试试语音识别的效果 ![[a/knot-20261117-155050-cdfe-42.m4a]] 
```

离线按下的记录先存在设备里，连上后按原来的时间补传。每一步都是「先可靠保存，再确认」：电脑确认写进日记后
设备才删除，重传会被去重。所以链路哪里断了，最坏只是晚一点写入，不会丢也不会重复。

## 设备

| 按键 | 记录模式（默认） | 浏览模式 |
| --- | --- | --- |
| 短按 OK | 记下这一秒 | 记下这一秒 |
| 长按 OK | 记下这一秒并开始录音（再按 OK 停止，最长 60 秒） | 同左 |
| 短按 UP | 记下这一秒并加 `#START` | 上一页 |
| 短按 DOWN | 记下这一秒并加 `#END` | 下一页 |
| 长按 UP / DOWN | 进入浏览模式 | 回到记录模式 |

短按长按在松手时判定，但时间取按下那一刻。屏幕列出最近 32 条记录，按日期分组（绿色已同步，黄色还在设备里）。
30 秒无操作关背光并回到记录模式。电源键是硬件开关，固件感知不到。

录音：16 kHz 单声道、4 位 IMA ADPCM，存在 4.5 MiB 的 `audio` 分区（9 段 × 60 秒）。时间记录之后上传，校验 CRC，
以 `.m4a` 存进库的附件文件夹，并用 Whisper（`mlx-whisper`，Apple Silicon）在本机转写。静音、低置信度片段和字幕套话幻觉会被丢弃，音频始终保留。

### 刷固件

从 Releases 下载 `FoloToy-AI-Passport-full.bin`，或自己构建（见下）。

- 首次安装或整片刷新：把合并固件写到 `0x0`（`esptool.py --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin`），会清掉配对。
- 保留配对和未同步记录的升级：`tools/flash-mac.sh <合并固件> [串口]`，只写 bootloader、分区表和应用三段。

## macOS 接收端

1. 从 Releases 下载 `KnotReceiver-<版本>.dmg`，把 **KnotReceiver** 拖进「应用程序」。
2. App 是本地临时签名，没有经过苹果公证。第一次打开请右键 → 打开，或运行
   `xattr -dr com.apple.quarantine /Applications/KnotReceiver.app`。
3. 打开后选择你的 Obsidian 库。它会注册当前用户的后台服务：登录时自动启动，异常退出 10 秒内自动重启。
   系统询问蓝牙权限时点允许；设备屏幕显示 `Pairing PIN` 时，在 macOS 弹窗里输入那 6 位数字。
4. 再次打开 App 可以查看状态、更换库，或停止并移除后台服务。

日志：`~/Library/Logs/KnotReceiver/receiver.log`。配置：`~/Library/Application Support/KnotNotes/config.json`。

## Windows 接收端

```powershell
uv venv --python 3.11 .venv
uv pip install --python .venv/Scripts/python.exe -r receiver/requirements.txt
./receiver/start.ps1 -Vault "C:\path\to\vault"
```

首次配对在终端里输入设备显示的 PIN（Bleak 在 Windows 上自带的配对只支持「点确认」，固件不接受）。不会安装开机自启。

## 写到哪里

- 日记路径读取 Obsidian 核心插件「日记」的设置（`.obsidian/daily-notes.json` 的文件夹和日期格式）；没有设置时是库根目录的 `YYYY-MM-DD.md`。
- 在 `## Log`（可配置 `heading`）下按时间插入，排在你手写的 `- HH:MM` 条目之间；已有的行从不改动。
- 当天日记还没建立时不写，记录留在设备里，等你建好日记（用你自己的模板）后再补入。过去某天缺日记则新建最简文件。
- 设备从没校过时就断电的记录，无法确定时间，写进 `<folder>/时间待核.md`，不编造日期。
- 去重账本：`<folder>/.knot-ledger.tsv`（默认文件夹 `Knot`）。

配置项（JSON，除 `vault` 外都可省略）：`vault`、`folder`、`diary_format`（moment 格式，如 `[Journal]/YYYY/MM/YYYY-MM-DD`）、`heading`、`address`、`attachments`（默认用 Obsidian 的附件文件夹）、`transcribe`（默认开）、`whisper_model`（默认 `mlx-community/whisper-large-v3-turbo`，首次启动下载约 1.6 GB）。

## 实现

| 层 | 技术 |
| --- | --- |
| 固件 | ESP-IDF 5.5.3、NimBLE、LVGL、官方 AI Passport BSP |
| 存储 | 256 KiB NVS 分区 `knots`（`0x7c0000`，512 条待传 + 32 条历史）；4.5 MiB `audio` 分区（`0x310000`） |
| 时间 | 微秒单调计时 + 电脑下发的锚点（三次蓝牙往返取最快一次的中点），每小时重校 |
| 链路 | 蓝牙服务 `6e410001-b5a3-f393-e0a9-e50e24dcca9e`，LE Secure Connections + 防中间人，设备显示 PIN，绑定 |
| 协议 | 每行一条 JSON：`hello` → `ping/pong` ×3 → `sync/synced` → `event` →（录音：`aget` → `a`… → `aend`）→ 按编号 `ack` |
| 接收端 | Python 3.11 + Bleak 3（CoreBluetooth / WinRT），原子写文件，intent/done 账本，mlx-whisper |
| macOS App | PyInstaller 打包，带蓝牙用途说明；launchd `RunAtLoad` + `KeepAlive` |

精度：时间源是电脑系统时间，加上半个蓝牙往返（约 30 毫秒），所以秒级可信，不保证亚秒。某次开机还没校时就关机，那次的记录无法确定时间。

## 构建与测试

```bash
bash tools/test_knot.sh          # C 模型/存储测试 + 接收端测试
bash tools/validate.sh --all     # 完整门禁，含 ESP-IDF 5.5.3 固件构建
receiver/macos/build_dmg.sh 0.1.0
```

ESP-IDF 不接受带空格的路径，必要时复制到别处构建。

## 来源与许可

基于 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport)（MIT），基线 `1c9e8b12c9c11a48419ea6c322c8a5d386d8ff22`。
蓝牙传输（`main/buddy_ble*`、`main/buddy_line*`）取自官方 BLE Buddy 参考代码，更换了服务 UUID 和应用协议。
上游 demo 保留在仓库中供参考。MIT 许可，见 [LICENSE](LICENSE)。
