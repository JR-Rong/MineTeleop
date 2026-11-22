# 关键实时视频的丢包恢复周期

八路 H264 的 Windows mock 现场复测中，四路发生短时 RTP 丢包，呈现帧龄达到约 526 ms，触发原有 500 ms 视频制动门限。该时刻输入、ACK 和校时仍健康；本轮连续长稳失败，守卫停止正确，不能将恢复前后的时长相加。

实时编码原先使用一秒 GOP。NACK、RTX、PLI/FIR 协商成功并不保证每次反馈和关键帧都能及时送达；反馈或请求后的 IDR 再次丢失时，周期 IDR 的等待本身就可能超过视频门限。现在关键相机按车端启动配置的 `critical_camera_stale_brake_ms`，为周期 IDR 分配一半时间，向下取整为帧数，至少一帧、最多保留原来的一秒 GOP。30 FPS、500 ms 对应 7 帧，约 233 ms。非关键相机保持原周期。

此修改不改变制动、输入、ACK、校时门限，不将收包或解码计数伪装成已呈现画面。会话中进一步收紧视频门限仍按收紧后的值制动；编码周期在本次媒体管线启动时确定。更频繁的 IDR 会改变码率分配，画质及真实网络长稳仍需复测。

## 可复现的合成丢包对照

显式启用 `MINE_TELEOP_BUILD_BENCH_PROBES=ON` 构建 `mine-teleop-video-loss-recovery-probe`；该工具不加入自动 CTest、不读取相机或 CAN。它需要可用的 NVENC 和 H264 解码器，生成五秒 720p30、125 kbps 视频，在 RTP 帧 35–38 注入同一短时丢包，并可阻断关键帧请求反馈。

```sh
mine-teleop-video-loss-recovery-probe --legacy-gop --drop-keyframe-feedback
mine-teleop-video-loss-recovery-probe --drop-keyframe-feedback
mine-teleop-video-loss-recovery-probe --legacy-gop
mine-teleop-video-loss-recovery-probe
```

用与现场包相同的 GStreamer、插件和动态库运行，保存 JSON 中的实际运行版本、丢包数量、解码输出间隔、IDR 周期及故障是否真正注入。未丢包、未解码或未正常 EOS 均不能算有效对照；修复策略超过 500 ms 时工具失败。旧策略允许输出超限结果以便比较。

本次 GStreamer 1.28.5 / NVENC / NVDEC 的初步对照中，反馈被阻断时原 GOP 的最大解码输出间隔约 867 ms，7 帧 GOP 约 172 ms。这是合成解码恢复证据，不能代替驾驶端呈现、真实网络、控制闭环或连续 210 分钟验收。
