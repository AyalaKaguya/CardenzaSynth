# HITS 格式 8 与示例重建验证

日期：2026-11-13。验证固件基于 `feat/mosaico-native` 的 `29f9946` 加本轮功能改动构建，固件身份见下表。

## 结果

- 工程及自动保存统一写格式 8，保留音符 HITS 和各轨 NEW HITS；继续读取格式 1–8。
- HITS-only 修改会改变工程指纹。格式 7 旧文件加载后默认每音一击，重存时升级为格式 8。
- 清理本地 108 份旧工程/自动保存文件；设备端删除 9 个旧工程及 1 个旧自动保存，并确认工程目录为空。
- 新建并上传 5 个示例：NeonCircuit_Drop、Intro、Break、Build，以及 HITS_Demo。
  四个 Neon Circuit 文件保持上游旋律和音色，每轨 8 个 Pattern、每份 285 个音符。
  HITS_Demo 为 100 BPM、8 个鼓点、1–4 次连击、NEW HITS 2。最终设备文件逐个下载并核对 SHA-256，再通过 C++ 存储读取与往返验证。
- 实机把 HITS_Demo 第 1 步底鼓从 HITS 1 改为 2：自动保存、手动 SAVE、重启后的新会话捕获均保存了 2；NEW HITS 2 也保留。
  验证后恢复了原始 1–4 次演示工程，设备最后停止播放，位于 SEQ / T1 / DRUMS。

## 测试与构建

7 项相关主机测试全部通过：CardenzaCoreTests、CardenzaHitsTests、CardenzaStorageTests、
CardenzaGrooveUiTests、CardenzaAppLifecycleTests、CardenzaNoCardLifecycleTests、CardenzaVolatileBootTests。
其中覆盖格式 7 兼容、格式 8 往返、只改 HITS 的指纹、自动保存恢复及教程正常/超时退出恢复。
架构检查、差异格式检查与 Python 语法检查通过。

本次使用官方 ESP-IDF master `4109c8923a51a5234801c324b39240d74e4cc379`（6.3），
构建前同步远端及所需子模块；独立 Python 6.3 环境保留了原有 6.2 环境。
原生应用未使用 IMU/磁力计，但 BSP 默认依赖的 BMI270 预编译库只有到 IDF 6.2。
新增本地 BSP 外设配置，直接编译固定 BSP 的电源、显示、音频、NAND 等原始驱动，
不加入运动传感器源码/API，也没有修改子模块或下载组件。

完整构建成功，应用 2,166,608 字节。构建日志共 5 条警告：可选 Iris 命令扩展、
原有的三个未使用函数/变量、以及应用超过 vibe_mode 分区大小的提示。
应用实际安装到 main_app，尺寸检查通过；更新包不含 bootloader 或 Vibe Mode 固件。

## 设备身份与更新

| 项目 | 值 |
|---|---|
| Device ID | `4553502d4952495301001c2904d088f4` |
| 最终 Boot ID | `5443223099755162` |
| 固件状态 | `normal` / healthy，crash_count = `0` |
| 更新操作 | `f96563b6-132d-4cc5-a7c0-a62160edf283` / `succeeded` |
| 最终 ELF SHA-256 | `011fc7b7c8f8bb2a2cc53508fc732f1ccf7e6fc5d4de63a07e8c62d22a9ad84a` |
| BIN SHA-256 | `f059cbbbc91321f52545fef914b5bf3562dcb2cd28be05fdbd839e3b7a4e457c` |
| Bootloader SHA-256（前后相同） | `3c1bdd7d58cd49ece060027f666a5f557564ea799ccda8822813b7dfec38598c` |
| 分区表 SHA-256（前后相同） | `8d28cdded305dd5f31c4a2aec315e4577e8e55f8ade887775d543306e6eb59a6` |

app-update 因本机缺少历史基础固件验证记录在写入前拒绝。
通过 `mosaico.py iris system-update --project Devices/MosaicoNative --skip-build` 使用已校验的本地更新包完成更新。
保留了历史 core dump 和日志；历史 dump 的固件身份与当前固件不同，未归因于本轮改动。

短时示例播放产生 1025 个非零输出块，最终 write_failures = 0，
estimated_underruns = 0。这是软件计数和短时运行验证，不代表扬声器听音或长时间满载验收。

## 产物与证据

以下证据和固件保存在本地忽略目录，不随源码提交；示例可通过仓库中的导出脚本重新生成。

- [示例及清单](../outputs/neon-circuit/projects-20261113T075237Z/manifest.json)，同目录的 readback 保存设备下载副本。
- [实机最终画面](../outputs/hits-format8-final.png)。
- [手动保存的工程](../outputs/hits-format8-manually-saved.ccproj) 与 [重启后捕获的工程](../outputs/hits-format8-auto-after-restart.ccproj)。
- [设备删除记录](../outputs/hits-format8-device-deletions.json)、[上传记录](../outputs/hits-format8-device-uploads.json)、[最终回读](../outputs/hits-format8-final-readbacks.json)。
- [主机测试 XML](../outputs/hits-format8-tests.xml)、[最终设备状态](../outputs/hits-format8-final-device-status.json)、[更新结果](../outputs/hits-format8-system-update.json)。
- [完整构建日志](../.codex-runs/mosaico/20261113T074957Z-game-build/build/20261113-154957-build-87724/raw.log)。
- BIN、ELF、map 和 irisfw 均保存于 `outputs/hits-format8/firmware/`。

重新生成示例：`python tools/demo/export_projects.py`；输出为新的时间戳目录，已有目录会被拒绝。
旧固件若最多支持格式 7，将无法打开本版的新存档。
