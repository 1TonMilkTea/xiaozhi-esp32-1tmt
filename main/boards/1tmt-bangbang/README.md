# 1tmt-bangbang：拾音律动 + 歌名显示

小智小车在本板子上新增两块能力：

1. **拾音跟随**：麦克风对着电脑喇叭拾音，OLED 画音量柱，小车在约 15cm 直径内跟着节奏扭动。
2. **正在播放**：电脑把 Windows 系统媒体会话（SMTC）里的歌名、作者推到设备，OLED 滚动显示 `歌名 - 作者`。歌名不是从喇叭听出来的。

电脑端推送代码在若依工程，见  
`RuoYi-Vue/ruoyi-admin/src/main/java/com/ruoyi/web/xiaozhi/README.md`。

## 目录

```
main/boards/1tmt-bangbang/
  README.md                 本说明
  compact_wifi_board.cc     板级入口：按键、MCP、电机、拾音开关
  compact_wifi_board.h
  motor_controller.h        电机控制
  config.h / config.json    引脚与板配置
  sound_follow/             拾音跟随（新增模块）
    sound_follow.h
    sound_follow.cc
```

公共模块里为这两项功能做的对接（不要挪出原目录）：

| 文件 | 改动 |
|------|------|
| `main/audio/audio_service.*` | 从麦克风 PCM 计算音量 RMS / flux |
| `main/display/display.*`、`oled_display.*` | 音量柱、`SetNowPlaying` 状态栏滚动 |
| `main/web_server/web_server.*` | `POST /api/now_playing` |
| `main/application.*` | `PulseMotor` / `StopMotor`；WiFi 后网页服务常开 |

## 拾音跟随

- **长按 BOOT**：开/关。
- **语音 / MCP**：`self.sound_follow.start`、`self.sound_follow.stop`。
- 空闲时才做动作；开始对话会自动停电机、关掉音量柱。
- 动作：短促左右扭 + 前进立刻等时长后退；航位推算超出半径约 7.5cm 会往回拉。

## 正在播放

设备连上同一 WiFi 后，网页服务在 80 端口常开：

```http
POST /api/now_playing
Content-Type: application/json

{"title":"歌名","artist":"作者"}
```

OLED 状态栏显示 `歌名 - 作者`；空内容时拾音跟随中显示「拾音中」。  
默认眼睛动画模式可能挡住文字，开拾音跟随后会切到普通显示。

## 使用

1. 电脑与小车同一 WiFi。
2. 在若依 `application.yml` 填 `xiaozhi.now-playing.device-ip`。
3. 重启若依；网易云 / QQ 音乐 / Spotify / Chrome YouTube 等会上报 SMTC 的播放器才能出歌名。
4. 长按 BOOT 开拾音跟随，把麦克风对着喇叭。
