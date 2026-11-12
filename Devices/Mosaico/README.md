# ESP-Mosaico 最小移植

此目标复用现有 Applications / OS / Library，在 ESP32-S31 上接入 Mosaico BSP 的屏幕与 ES8311 音频输出。它是独立设备后端，不接入 OP-Ay1 工作站的音频主循环。

## 本轮范围

- 保留 **240×135 原始 UI，1:1 显示**，在 480×480 屏幕上放于 `(120,172)`，其余区域为黑色；不缩放或重排页面。
- 系统每 10 ms 调度一次，隔帧绘制，目标刷新周期 20 ms；实际帧耗时需真机测量。
- 使用现有 44.1 kHz 合成器、音序器与 MIDI 路由，输出经唯一的 BSP codec/I2S 后端；原单声道信号复制到左右声道。
- 键盘及触控键盘、USB-MIDI、USB Audio、麦克风输入、NAND 文件存储均未接入。不会挂载或格式化 NAND，也不保存工程或设置；本轮使用易失会话，启动时不弹出必须用按键关闭的无卡提示。
- USB 保留给 ESP-Iris 管理链路；应用的矩阵按键与按键灯接口暂只提供空输入和 RAM 状态。
- 提供只读 Iris 截图和运行计数；截图来自显示任务完成传输后的快照，480×480 黑底映射原画面，不是面板读回。未注册触控或远程键盘输入。

默认启动音量为 **20%**，在系统启动后通过现有 Juno 轨播放一段短 C–E–G 音序，再停止并清除测试音符。可在 `Cardenza Mosaico minimal port` 配置中关闭 `CONFIG_CARDENZA_MOSAICO_AUDIO_TEST`，或调整 `CONFIG_CARDENZA_MOSAICO_TEST_VOLUME`。软件诊断可确认非零 PCM 与 codec 写入，不能替代扬声器听音验收。

## 依赖版本与构建记录

| 组件 | 本次使用的提交 |
|---|---|
| Vibe 工作区 | `d6aa4a8d3277b0c7ad3d38e54c403f2388814d1e` |
| Utils / ESP-Iris / 保留固件集成 | `be151ae047014e79a9c7ae0092dd203844499003` |
| Mosaico BSP | `5f88b91e1be055d1d3674364123b2779a1758377` |
| AMY | `0427e3b58172415edf6b0f504220ab6c79987baa` |
| 官方 ESP-IDF master 调查快照 | `0e6b480f858c69862ffb6001d96e9eef74599a6e` |

Vibe 位于本仓库的 `tools/mosaico-vibe` 子模块。这里采用 Vibe 固定的 BSP，未混入 BSP HEAD 的新 NAND 接口。

ESP-IDF 始终使用官方 master 最新代码，覆盖 Vibe 历史 SDK 固定值。每次构建前拉取远端、更新 SDK 及所需子模块，再记录本次 SHA；表内 `0e6b480…` 仅为早期构建记录，不是后续构建约束。不能只凭目录名 `master` 或 `6.2.0` 版本字符串判断 SDK 已是最新。

## 构建

在本仓库根目录的 PowerShell 中运行，复用已安装的工具与 Python 环境：

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:IDF_PATH = 'C:\esp\master\esp-idf'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\python_env\idf6.2_py3.11_env'

uv run --no-project --python C:\Espressif\python_env\idf6.2_py3.11_env\Scripts\python.exe mosaico.py game build --project Devices/Mosaico --target native --idf-path C:\esp\master\esp-idf
```

根目录 `mosaico.py` 选择本仓库固定的 Mosaico 工具。目标是普通 ESP-IDF 应用，已有自己的 `app_main()`；此构建不引入 Raylib，也不安装到设备。运行前应确认 AMY、Vibe 及其所需 BSP / Utils 子模块已初始化，所选 SDK 支持 `esp32s31`。

新 checkout 只需初始化本目标需要的依赖：

```powershell
git submodule update --init Library/third_party/amy tools/mosaico-vibe
git -C tools/mosaico-vibe submodule update --init submodule/esp-mosaico-bsp submodule/esp-mosaico-utils
```

只生成离线 `.irisfw` 更新包、不连接设备：

```powershell
uv run --no-project --python C:\Espressif\python_env\idf6.2_py3.11_env\Scripts\python.exe tools/mosaico_bundle.py --idf-path C:\esp\master\esp-idf
```

输出在 `Devices/Mosaico/build/`；保留同一构建的 BIN、ELF 和 map。初次使用时打包工具会准备该固定版本的独立 Iris 主机依赖环境。

## 最小设备诊断

已选定设备并建立 Iris 会话后，使用根目录产品 CLI：

```powershell
$env:PYTHONIOENCODING = 'utf-8'
uv run --no-project mosaico.py iris device-status --project Devices/Mosaico --inventory --json
uv run --no-project mosaico.py iris screenshot outputs/mosaico/device.png --project Devices/Mosaico --json
uv run --no-project mosaico.py iris rpc 17219 1 --project Devices/Mosaico --json
uv run --no-project mosaico.py iris rpc 17219 2 --project Devices/Mosaico --json
```

服务 `0x4343`（17219）的方法 1 返回 JSON schema 1 状态，包括帧计数、内存、音频写入和测试结果；方法 2 只排队一次同样的短乐句，运行中拒绝重复请求。实际播放在原系统任务执行，不在 Iris RPC 线程操作音序器。两次状态差分可以核对系统和音频进度；`estimated_underruns` 是队列时间估算，不是硬件中断计数，`runtime_stats:false` 时不能据此给出整机 CPU 占用率。

应用采用保留固件的 Recovery ABI 2 与 `mosaico-retained-test-2m-v2` 分区合同，保留固定启动分区前缀，OTA 写入者仍在 Vibe Mode。不得使用 Cardenza 的分区表覆盖它。设备安装与更新须遵循 Mosaico 的设备操作流程，通过本仓库 `mosaico.py` 选择匹配的更新方式；不要直接执行 `idf.py flash`、`esptool` 写入或猜测偏移。本次已通过现有 Vibe Mode 完成应用安装。

## 验证状态

2026-11-11，CardenzaSim 的 Debug 全目标构建成功，**47/47 主机测试通过**，共 170.57 秒，包括易失启动、无卡生命周期、Groove UI、采样、截图回放和 HAL 故障注入。日志与 JUnit 位于 `build/mosaico-host/`。

Mosaico 目标已在官方 master `0e6b480f858c` 下编译与链接成功，最终诊断版应用 BIN 为 1,990,560 字节（约 1.90 MiB）。最终固件构建记录有两条工具提示：IDF 默认不加载 Iris 的可选命令扩展，以及应用大小超过保留的 `vibe_mode` test 槽；本应用实际目标为 `main_app`（0x210000、0xdf0000），尺寸检查通过，不写入 `vibe_mode`。

离线 Vibe Mode 更新包已生成，并用官方 Iris bundle inspector 校验为 `esp-iris-system-update/0.2`：仅包含分区表与 `main_app` 应用，不含保留固件或 bootloader。它是本地开发用未签名包；BIN、ELF、map、包清单和 SHA-256 汇总保存在 `outputs/mosaico/`。

实机基础验收已完成：同一 S31 设备经 Vibe Mode 更新为目标应用，现场确认原始画面与短乐句；Iris 截图、启动/重放自测和约 109 秒计数观察通过，无新增音频写入失败或断流估计，内存余量保持稳定。键盘外设、低内存降级和持续满载性能仍未验收。详情及证据见 [2026-11-11 实机报告](../../docs/mosaico-device-validation-20261111.md)。
