# Mosaico 原生界面

`MosaicoNative` 是独立于 `Devices/Mosaico` 最小移植的原生界面目标，使用
`CARDENZA_NATIVE_UI` 接入触控、麦克风和应用存储。最小移植仍保留原始界面。

## 业务代码边界

- `Applications/Synth/GrooveApp` 管理应用生命周期、输入活动、会话与界面状态。
- `UI/Groove/Native/NativeView` 管理绘制和触控命中；`NativeScenes`、
  `NativeModals` 分别组织场景与弹窗，`NativeActions` 将动作交给共享控制器。
- `UI/Groove/Render/SceneComponents.h` 和 `ModalComponents.h` 定义共享绘制组件，
  由紧凑界面和原生界面提供各自布局；绘制组件不派发业务动作。
- `Runtime/Sampler/SamplerManager` 管理采样轨道状态；`NativeSamplerJobs`
  执行后台保存和波形计算，通过轮询结果在应用任务中更新状态。
- `OS/Storage/Settings` 捕获设置编辑的独立快照，后台写入后由系统任务确认；
  后台任务不直接访问设置对象或注册表。
- 本目录的 `Touch`、`Microphone`、`Storage` 负责设备适配，合成器、音序器和
  控制器继续由共享业务层提供。

## 原生钢琴窗

SEQ 的 ROLL 使用 8 步 × 8 音高触控网格，TIME 按钮每次移动 8 步，
PITCH 按钮每次移动 8 个半音。点格子添加或删除该步的对应音符；
点顶部步号按共享规则选择或取消编辑步，点底部编辑项切换 NOTE / LEN / VEL / TIME / HITS，
由 ADD / SUB 调整所选步。音符条显示时值、微步偏移和连击，播放游标标出当前步。
STEP 仍使用步进视图。当前不提供拖动音符或拖动时值手柄。

音符编辑经 `native::Command` 的绝对步号与 MIDI 音高交给共享控制器，
不将屏幕坐标传入音序器。时间窗口、选择和播放跟随使用音序器的同一套状态；
Native 只配置 ROLL 每页 8 步，STEP 与普通界面默认每页 16 步。
TIME、ADD / SUB 和菜单翻页复用共享控制器的选择处理规则（单选翻页清除旧选择，多选允许跨页）。
音高滚动由视图维护，音符、时值、连击、裁剪和触控几何由共享钢琴窗组件绘制。

鼓轨 ROLL 的八行对应 BD / SD / CH / OH / CP / TM / CB / MA，不能写入无效鼓音。
左侧音名和鼓名可作琴键：选步后按住亮起的键，用 ADD / SUB 编辑其音符，
与实体 Pad 共用编辑流程；取消触点不会提交删除。旋律轨仍可滚动音高。

## 与共享界面的操作对齐

- 两侧快捷键仅显示图标和灯带，不绘制按键名称；触控区域与短按、长按操作保持原样。
- 左侧 ESC / TAB / FN / CTRL 短按复用共享控制器动作，长按 250 ms 打开并保持
  对应地图，再按同一键关闭。场景、轨道和页面选择后地图保持；菜单动作按其业务打开弹窗或执行。
- ESC 场景地图使用 2×2 卡片，点击整张卡片切换场景；卡片内页面列表仅展示，
  没有独立触控或滚动操作，长列表在卡片内分列排版。页面切换仍使用 CTRL 或底部分页标签。
  轨道地图保留选轨、MUTE、SOLO、SEQ，地图内 ADD / SUB 可切轨或翻页。
- 工程和预设 SAVE AS、工程与采样 RENAME 使用共享文字输入流程。
  触屏键盘提供大写字母、数字、空格、连字符、光标移动、退格和清空，确认与取消分别提交或放弃。
- 预设和采样浏览器提供 SEARCH，过滤条件由共享浏览器逻辑处理。
- SESSION 点击片段直接启动或排队；MIX 的 MUTE / SOLO / SEQ 点击直接切换；
  音色页动作和工程 SAVE / LOAD / NEW 点击直接执行，连续参数仍先选中后用 ADD / SUB 调整。
- ENTER 始终进入共享控制器的确认流程（SESSION 为启动整行），直接点击动作格使用对应共享动作，
  不另建一套 ENTER 语义。触屏键盘与矩阵键盘共用字符编辑与校验；弹窗打开期间仍显示错误提示。
- 重命名采样若改变当前工程的任一轨道文件引用，会标记工程已修改，切换工程时提示保存。

## 工具入口

从仓库根目录使用 `mosaico.py`，沿用[最小移植的依赖与 SDK 配置](../Mosaico/README.md)，
将项目参数设为 `--project Devices/MosaicoNative`。
每次构建前同步官方 ESP-IDF master 最新提交及其子模块，再记录本次 SHA；
此前的 SHA 只用于追溯，不约束后续构建。SDK 更新后使用独立构建目录重新编译。
`mosaico.py native-files --help` 和 `mosaico.py native-touch --help` 分别说明文件与触控工具。
离线打包入口为 `tools/mosaico_bundle.py --project Devices/MosaicoNative --idf-path <SDK路径>`。

音频状态 RPC（`0x4343`，方法 `1`）只读，不启动播放。`volume` 是保存的主音量，
`startup_muted` 和 `recording_muted` 分别说明启动与录音的输出门控；两者不修改主音量。
`nonzero` / `peak` 统计音量处理前的合成器 PCM；`output_nonzero` 只统计成功写入
codec 的非零输出块，`output_peak` 是最近成功写入块的音量处理后峰值，
`output_gain_q15` 是该块末尾的增益（32768 为原幅度）。这些状态不能代替扬声器听音确认。

已有的最小移植实机验证记录不代表此目标已完成触控、录音、存储或性能验收。
