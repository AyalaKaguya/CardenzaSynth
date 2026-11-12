# CardenzaSynth → ESP-Mosaico 适配分析

核查日期：2026-11-11。范围：当前 CardenzaSynth、桌面 OP-Ay1 工作区、上游 Vibe / BSP / Utils 与 ESP-IDF。用户指定使用官方 ESP-IDF master 最新开发版，覆盖 Vibe 指定的历史 SDK SHA。

本文件记录前期静态分析与后续方案调整。当前实施范围已经按用户要求收敛为最小后端：系统调度、240×135 原图 1:1 输出与 BSP 音频，不接键盘或存储，暂不重构 480×480 UI。实际版本、构建和验证状态以 [Mosaico 最小移植说明](../Devices/Mosaico/README.md) 为准；下述完整功能与响应式 UI 内容属于后续路线。

## 结论

技术上可行，推荐新增 `Devices/Mosaico` 后端，复用 Applications / OS / Library。根据后续提出的原生 480×480 响应式方向，推荐共享音乐核心与绘图控件，保留原设备 240×135 布局，并为 Mosaico 增加原生 480×480 触控布局。旧画面放大与虚拟矩阵只作为可选的早期对照工具，不作为最终产品 UI。

可复用四轨音源、Pattern / Song、和弦与琶音器、混音效果、采样管理、工程格式及自动保存。主要工作是平台适配，以及共享代码中少量 Cardenza 硬件假设的参数化。不能用“复制 CardenzaHAL 并改引脚”的方式完成。

硬件能力和已查 DSP 实现没有显示出根本阻断；四轨复音、采样流和效果器并发的实时余量仍必须在 S31 上测量。

## 版本与升级边界

以下远端提交由本轮 `git ls-remote` 或 GitHub tree API 核对；它们是这次调查的快照，不代表以后一直最新。

| 项目 | 本机当前 | 本轮核实的上游 / 建议 |
|---|---|---|
| OP-Ay1 所用 Vibe 工作区 | `5845064`，另有未提交规则、文档修改 | Vibe main `d6aa4a8d3277b0c7ad3d38e54c403f2388814d1e` |
| esp-mosaico-utils | `95e1e8fb0334b975365a37ca09b448f9c581b649` | 最新 Vibe 固定 `be151ae047014e79a9c7ae0092dd203844499003` |
| esp-mosaico-bsp | 本地分支 `479bf2a7254ca847506bf06a3ae9ae0e0b69dd2d`；父仓库固定 `5ae6e910…` | 最新 Vibe 固定 `5f88b91e1be055d1d3674364123b2779a1758377`；BSP 远端 HEAD 为 `716fc7bca30ed9c61166a0343bf6f5f995b5131f` |
| ESP-IDF | `C:\esp\master\esp-idf`，master `383dda6c182e3dabf6355160166fe007df775629`；源码版本 6.2.0 | 官方 master `0e6b480f858c69862ffb6001d96e9eef74599a6e`，源码版本仍为 6.2.0 |

升级应先保留 OP-Ay1 当前工作区、BSP 指针和已有修改，在独立的移植工作区或分支整合新 Vibe。建议第一条基线采用最新 Vibe 固定的 Utils / BSP 组合；若要用 BSP HEAD 的新 NAND 接口，将其作为单独升级项评估，不能把两种依赖组合混称为同一基线。

本地 BSP 的 lazy RX 行为只在录音消费者需要时开启麦克风接收；远端 BSP HEAD 初始化会开启 RX。升级时要保留或重新评估这一行为，并回归录音启停、空闲资源占用、播放稳定性。不要用 submodule 强制更新覆盖本地修改。

Vibe 官方规则目前指定 IDF `7b9cc1ac…`；本次按用户要求覆盖。已核对的构建入口通过 `IDF_PATH` 选择 SDK，doctor 检查版本、目标和记录实际 SHA；所查路径没有要求自动切回指定 SHA。BSP 依赖要求 `idf >=6.2`、目标为 `esp32s31`。因此更新策略应为：

1. 核对现有 master SDK 的本地修改，保留稳定 SDK 与现有工程。
2. 将开发 SDK 整合到实施当日官方 master 的具体 SHA，并同步它要求的子模块、编译器和 Python 依赖。
3. 显式选择 `IDF_PATH`，核对 `esp32s31` 支持、组件依赖和 doctor 结果；不把版本字符串 6.2.0 当作同一 SDK 的证明。
4. 移植期间固定经过验证的 master SHA；下一次更新独立做回归。配置、构建日志与产物清单记录 Vibe / Utils / BSP / AMY / IDF 的实际提交。
5. 将工作区规则和文档中的 SDK 口径统一为这项用户选择，保留目标、依赖和分区检查，不关闭兼容性验证。

## 平台差异与处理方式

| 项目 | CardenzaSynth 当前 | Mosaico 适配建议 |
|---|---|---|
| CPU | ESP32-S3 / Xtensa | ESP32-S31 / RISC-V；使用通用 C DSP 后测性能，不沿用 S3 指令与缓存结论 |
| 显示 | 240×135 RGB565 | BSP 为 480×480 RGB565 CO5300；保留逻辑帧缓冲，在呈现层缩放 |
| 输入 | 4×14 实体键及按键灯 | CST9220 触控与少量板载按键；虚拟矩阵、可见修饰键锁定、按触点维护状态 |
| 音频 | 44.1 kHz，核心最终为单声道 | BSP / ES8311 默认 48 kHz、16bit、2ch，支持自定义 I2S 配置；先验证 44.1 kHz，全链路保持一致 |
| 存储 | SD 卡 / FAT，启动时确定可用性 | SPI NAND / LittleFS，映射应用专属目录；沿用项目内容格式，调整后端与提示语义 |
| 内存 | S3 专用 PSRAM 初始化与快速内存预留 | 由 IDF / BSP 初始化，按能力分配 DMA / 内部 RAM / PSRAM；阈值按 S31 实测 |
| USB | CardenzaHAL 的 MIDI / Audio / HID | 尊重 ESP-Iris 所有权；MIDI Host 与 Iris device 需要独占切换 |
| 启动与更新 | Cardenza Launcher 分区 | 接 Mosaico retained ABI / system-update；使用 Mosaico 分区合同 |

OP-Ay1 已有构建配置记录双核 320 MHz、16 MB NOR、Octal PSRAM。这是已有配置证据，不是本轮实机测量；PSRAM 总量、可用内部 RAM、DMA 能力和性能应从目标固件与设备读取。

## 建议代码组织

```text
CardenzaSynth/
  Applications/Synth/        复用应用与音乐业务
  OS/                       复用生命周期、MIDI、存储及绘图接口
  Library/Synth/             复用音源与DSP
  Devices/Mosaico/
    Device.cpp              显示、触控输入、虚拟灯光、存储、后台服务
    DeviceAudio.cpp         BSP codec/I2S唯一音频后端
    app_main.cpp            初始化、Iris、任务与应用生命周期接线
    CMakeLists.txt          Mosaico目标组件
```

在 Vibe 工作区的 `projects/cardenza-synth` 建立独立应用外壳，引用固定版本的 CardenzaSynth 源码；首版可用显式源码路径，稳定后固定为 Git 依赖。不要复制一套核心到 OP-Ay1 后各自修改，也不要把该适配塞进现有 OP-Ay1 的音频主循环。

新增目标采用普通 ESP-IDF 应用集成，接入 `esp_mosaico_app_recovery` 和 system-update 入口；不需要引入 Raylib。原生响应式像素 UI 可以直接使用 BSP panel / touch 接口，不要求迁移为 GSP 或 LVGL。保留现有色彩、字体与 dither 风格，并以实际像素重新绘制控件。

## 必须拆开的共享代码假设

- `Devices/Device.h` 固定 240×135 和 4×14；将显示尺寸改为由目标配置与实际 surface 提供，保留原后端的 240×135 配置。Mosaico UI 不再依赖矩阵尺寸决定布局。
- `SynthAudioRuntimeAmy.cpp` 在 `ESP_PLATFORM` 下包含 Cardenza 的双核 helper、任务优先级、渲染预算和 6×256 输出缓冲预估。把实际队列深度、任务 affinity 和预算变成目标策略，而非所有 ESP 板共用常量。
- `SynthSequencer.cpp`、`SampleStream.cpp` 有任务核与优先级设置，应与显示、codec、Iris 共同规划。音序时钟继续独立于 UI，不由屏幕刷新驱动。
- 不迁移 S3 的手动 MMU / PSRAM 注册、全局 new 策略、panic wrapper 和 `audio_hot.lf` 中固定库名与工具链对象名。保留 Fast / Slow 内存接口的意图，按新平台实现。
- `ValidateArchitecture.cmake` 的 Cardenza 专属“禁止内部存储 / NVS”规则需要限定目标。业务数据仍走 OS Storage，但 Mosaico 系统元数据所需 NVS 是其启动合同的一部分。
- 最新 IDF 的组件拆分、公开头文件和驱动依赖按新目标逐项声明，避免照搬旧固件的 `driver` 等聚合依赖。

## 音频实现

推荐先保留 44.1 kHz 与 256-frame 合成块，一个块约 5.80 ms。先验证 BSP 自定义 I2S 配置和 ES8311 codec 同时采用 44.1 kHz；若目标后端必须 48 kHz，在输出边界增加有状态重采样，不能只改采样率标签。

原核心输出单声道，首版需要时复制到 codec 的左右声道；不能据此宣称增加了立体声声像。音频设备、I2S 及 DMA 由唯一后端持有，避免 OP-Ay1 的 app_audio 与新后端重复初始化。

控制事件入队，音频任务按块消费；界面、自动保存和 NAND 读取不持有长时间渲染锁。采样流走后台预读，文件替换仍使用已有 SampleFileGuard。实际缓冲余量和 underrun 来自 Mosaico 后端，不能沿用 Cardenza 的估计。

AMY 固定提交 `0427e3b58172415edf6b0f504220ab6c79987baa` 已查到通用实现：FM 缓冲 PIE 汇编只对 Xtensa S3 启用，其他目标走库函数；定点乘法和 LPF24 有 C / int64 路径。尚未完整审计全部源码或用 S31 编译；64 位乘法成本与实际复音能力仍待测。

## 显示与输入实现

推荐将固定坐标渲染改为原生响应式布局。已有 `Gfx` 从传入的 `MutableFrameBufferView` 获取实际宽高，大部分波形、包络、推子和参数控件接受 `Rect`，可以继续复用；主要固定点是 `Theme.h` 中的几何常量、`Widgets.cpp` 的 Cell 计算和各 scene / modal 的像素坐标。

建议增加以下边界：

- `LayoutContext`：实际可用区域、输入能力、字体档位等。
- `ComputeLayout(context)`：生成内容区、导航、演奏区、参数卡、列表、弹窗等几何；布局在启动或尺寸 / 页面需求变化时重算，高频渲染不做反复堆分配。
- `CompactLandscape` / `SquareTouch`：首先明确支持原设备 240×135 与 Mosaico 480×480 两套布局策略，不承诺未经验证的任意屏幕比例。
- `HitTest`：直接使用布局输出的同一份控件矩形，保证绘制位置与触控命中一致；拖动捕获后按控件语义处理移出、抬起和取消。
- `UiAction`：矩阵键和触控均转为统一语义命令，例如选择轨道、切页、设置参数、播放、编辑 step、演奏 note-on / note-off。应用层统一处理状态变更和音符所有权，避免触控端绕开录音、通知或自动保存。

特别要调整 `GrooveApp::HandleMatrixKeyEvent` 中的自动保存触发：目前项目变更活动由矩阵事件标记，新触控操作必须通过共享操作入口产生同样的 dirty / sessionChanged 和活动通知。

480×480 建议按当前页面重新分区：顶部状态与四轨选择，中央场景编辑，底部走带 / 导航，演奏区按页面显示或展开。乐器页可用双列参数和较大的包络图；SEQ 的 16 步可按 8×2 排列并提供直接触控；MIX 使用四条可拖推子；Sampler 使用更大的波形与起止标记。字号、列表行高和命中区域按目标尺寸重新设置。以上为布局方向，具体像素和交互仍需预览确认。

原生布局不要求常驻完整 4×14 虚拟键盘。若早期需要回放对照，可以暂时将旧画面放大为 480×270，下方提供兼容键区；该方案与正式触控布局区分开。

当前触控驱动声明两点并提供 track ID；板上实际多点与稳定性仍需验证。输入适配必须按触点分别维护演奏音符与控件持有者，处理抬手、取消、失焦和页面切换。原矩阵组合操作需要在触控 UI 中提供可见替代，例如直接导航、工具按钮、先选音符再编辑 step。兼容键区可提供修饰键锁定；不要将单个 pointer 的 release 当作释放全部按键。

沿用 BSP 的 RGB565 字节序、CO5300 对齐和 TE 呈现要求。原生 480×480 的像素量约为 240×135 的 7.11 倍；单个 RGB565 缓冲为 460800 字节，两个约 900 KiB，需纳入 PSRAM、DMA 与带宽预算。采用适当的局部刷新与动画节制，按实测确定刷新率，音乐时钟不跟随 UI 帧率。不要为了全屏动画无条件复制多份画面。

## 存储与更新

建议挂载既有 NAND LittleFS 后，由 Mosaico 后端先确认或创建 `/nand/apps`，再将 Device 存储根设为该路径。现有 OS Storage 把 Device 根视作已存在挂载点，不会创建根本身；它随后拼接并创建 `CardenzaSynth/`，形成 `/nand/apps/CardenzaSynth/{Settings,Projects,Presets,Samples,Session,Logs}`。这是建议的新应用命名空间；不得迁移或覆盖已有 OP-Ay1 文件。

保留 `.ccproj`、预置和 WAV 格式，通过 OS Storage 封装后端差异。将面向用户的“无 SD 卡”改成适合 Mosaico 的存储状态，但不改变 Cardenza 目标提示。挂载失败不得自动格式化或扩容，写入失败不能假报保存成功。

BSP HEAD 新增 `bsp_nand_flash_init_blockdev()`，与 Vibe 固定 BSP 的 NAND 接口不同。若采用 HEAD，核对块设备所有权、LittleFS 版本、旧卷几何参数和失败清理后再集成；有新 API 不代表既有卷兼容已经验证。

使用 Utils 的保留固件合同：Recovery ABI 2、`mosaico-retained-test-2m-v2`、固定启动分区前缀。Cardenza 的分区表不能迁入。新版 IDF 构建普通应用，与替换设备上的 Vibe Mode 是两个独立动作；现有保留固件满足 ABI 时优先保持，更新它需独立验证启动、回退、身份与数据保留。

USB 首版保留 Iris 调试 / 更新链路。外接 USB-MIDI 键盘作为独立功能接 OP-Ay1 已有的所有权模式，先释放 Iris USB 并保留可用替代管理链路，再进入 Host；不假定 Host、USB MIDI device、USB Audio 和 Iris 能同时使用同一控制器。

## 方案取舍与实施顺序

| 方案 | 优点 | 工作与限制 | 建议 |
|---|---|---|---|
| A. 新后端 + 原像素 UI + 虚拟矩阵 | 核心改动小，可利用现有截图 / 回放与音乐测试 | 触控演奏空间有限 | 可选的早期验证工具 |
| B. 保留音乐核心与控件，开发 480×480 原生响应式触控 UI | 更适合 Mosaico，可直接触控音序和混音 | 需要布局参数化、统一语义输入、导航与编辑交互设计 | 根据后续需求，作为推荐产品路线 |
| C. 并入现有 OP-Ay1 工作站 | 能复用部分设备服务 | 两套音序、混音、采样与工程状态需整合，边界复杂 | 初次移植不优先 |

阶段 0：隔离整合最新 Vibe 与选定 BSP，选择并固定官方 master SDK，建立版本清单。最小应用须通过环境检查、构建和分区 / ABI 检查。

阶段 1：只接显示、触控、codec 与存储后端，验证画面、真实按下 / 抬起、固定测试音、文件原子写与重启读取。将硬件不兼容与音乐核心问题分开定位。

阶段 2：接 AMY 和四轨核心，复用现有主机回归；对固定音符与预置比较音高、响度、削波和波形特征，再验证采样播放、效果和音序时钟。

阶段 3：先在桌面预览确认原生 480×480 布局和交互，完成布局参数化与共享语义输入，再接 Mosaico 触控。覆盖所有场景和弹窗、命中检测、拖拽与取消、录音、保存、加载、自动恢复、存储失败与低内存行为；原 240×135 桌面和浏览器目标继续通过现有回归。

阶段 4：真机四轨 + 效果 + 采样流 + UI 连续操作压力测试，记录音频 underrun、渲染耗时、音序迟到、事件队列、内部 RAM / PSRAM 最大连续块及 NAND 延迟。先短测定位，再安排至少 30 分钟持续负载作为验收建议；不能用编译成功或声音能响替代。

阶段 5：在真机负载数据基础上调整局部刷新、动画与页面密度，完成触控易用性验收。工程与音频核心继续共用，避免改界面时再次改音乐语义。

## 主要证据入口

本地：`AGENT.md` 的 Architecture / Memory / Sound Rules；`Devices/Device.h`；`Library/Synth/Audio/SynthAudioRuntime.h`；`Library/Synth/Audio/Amy/SynthAudioRuntimeAmy.cpp`；`Library/Memory/Memory.h`；`OS/Storage/Storage.cpp`；`cmake/ValidateArchitecture.cmake`；OP-Ay1 的 `AGENTS.md`、`projects/OP-Ay1/components/app_nand`、`app_usb_midi` 和 BSP 公共头文件。

- [Vibe 最新已核查提交](https://github.com/esp-mosaico/esp-mosaico-vibe/tree/d6aa4a8d3277b0c7ad3d38e54c403f2388814d1e)
- [Vibe 的固定 SDK 规则](https://github.com/esp-mosaico/esp-mosaico-vibe/blob/d6aa4a8d3277b0c7ad3d38e54c403f2388814d1e/AGENTS.md)
- [已核查 IDF master 提交](https://github.com/espressif/esp-idf/commit/0e6b480f858c69862ffb6001d96e9eef74599a6e)
- [S31 官方 SDK 支持说明](https://esp32-s31.espressif.com/en/faq)
- [Vibe 固定 BSP 的依赖清单](https://github.com/esp-mosaico/esp-mosaico-bsp/blob/5f88b91e1be055d1d3674364123b2779a1758377/components/esp-mosaico-bsp/idf_component.yml)
- [BSP HEAD 的 NAND 公共接口](https://github.com/esp-mosaico/esp-mosaico-bsp/blob/716fc7bca30ed9c61166a0343bf6f5f995b5131f/components/esp-mosaico-bsp/include/bsp/nand_flash.h)
- [Utils SDK 路径解析](https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/mosaico-tools/tools/mosaico_cli/runtime.py#L143)
- [保留固件产品合同](https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/esp-mosaico-vibe-mode/product_contract.json)
- [AMY 非 S3 的 FM 缓冲回退](https://github.com/203-Systems/amy/blob/0427e3b58172415edf6b0f504220ab6c79987baa/src/algorithms.c#L71)
- [AMY 定点运算](https://github.com/203-Systems/amy/blob/0427e3b58172415edf6b0f504220ab6c79987baa/src/amy_fixedpoint.h#L226)
