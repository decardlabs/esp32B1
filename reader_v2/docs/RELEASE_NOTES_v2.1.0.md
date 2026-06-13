# Release Notes - v2.1.0

发布日期：2026-06-13

## 发布类型

小版本发布（功能增强 + 稳定性修复 + 文档同步）

## 主要变更

- 云端 TTS 增加预取流水线（prefetch ready/prefetch hit）
- 云端 HTTP 客户端启用 keep-alive 复用
- 修复播放启动竞态，消除假性 no data received
- 音频任务栈水位日志节流，避免高频刷屏
- 调整低内存条件下预取触发门槛，提升命中率
- 固件启动日志版本号升级为 Voice Reader v2.1.0

## 运行表现（实机日志）

- 连续出现 prefetch ready 与 prefetch hit，说明句间音频缓存命中有效
- 未再出现 audio_task: no data received
- 音频栈日志恢复低频输出

## 文档同步范围

以下首方文档已同步至 v2.1.0（2026-06-13）：

- README.md
- CLAUDE.md
- docs/ARCHITECTURE_REVIEW.md
- docs/REVIEW_SUMMARY.md
- docs/ESP32-S3稳定运行审查规范.md
- docs/ESP32-S3代码审查核心规范.md
- docs/火山引擎TTS调用说明.md
- docs/archive/PHASE1_PATCH.md
- docs/superpowers/specs/2026-06-07-voice-reader-crash-fix-design.md
- docs/superpowers/specs/2026-06-08-cloud-tts-design.md
- docs/superpowers/plans/2026-06-07-voice-reader-crash-fix-plan.md
- docs/superpowers/plans/2026-06-08-cloud-tts.md
- docs/superpowers/plans/2026-06-09-code-review-fixes.md

## 说明

- `components/` 目录下第三方依赖文档未改动，避免污染上游内容。
