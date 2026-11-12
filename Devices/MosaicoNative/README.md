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
点顶部步号选择编辑步，点底部编辑项切换 NOTE / LEN / VEL / TIME / HITS，
由 ADD / SUB 调整所选步。音符条显示时值、微步偏移和连击，播放游标标出当前步。
STEP 仍使用步进视图。当前不提供拖动音符或拖动时值手柄。

音符编辑经 `native::Command` 的绝对步号与 MIDI 音高交给共享控制器，
不将屏幕坐标传入音序器；时间与音高窗口由视图维护，供后续外设适配参考。

## 与共享界面的操作对齐

- 左侧 ESC / TAB / FN / CTRL 短按复用共享控制器动作，长按 250 ms 打开并保持
  对应地图，再按同一键关闭。场景、轨道和页面选择后地图保持；菜单动作按其业务打开弹窗或执行。
- 场景地图按四列显示，列内可直接选页面；轨道地图保留选轨、MUTE、SOLO、SEQ。
- 工程和预设 SAVE AS、工程与采样 RENAME 使用共享文字输入流程。
  触屏键盘提供大写字母、数字、空格、连字符、光标移动、退格和清空，确认与取消分别提交或放弃。
- 预设和采样浏览器提供 SEARCH，过滤条件由共享浏览器逻辑处理。
- SESSION 点击片段直接启动或排队；MIX 的 MUTE / SOLO / SEQ 点击直接切换；
  音色页动作和工程 SAVE / LOAD / NEW 点击直接执行，连续参数仍先选中后用 ADD / SUB 调整。

## 工具入口

从仓库根目录使用 `mosaico.py`，沿用[最小移植的依赖与 SDK 配置](../Mosaico/README.md)，
将项目参数设为 `--project Devices/MosaicoNative`。
`mosaico.py native-files --help` 和 `mosaico.py native-touch --help` 分别说明文件与触控工具。
离线打包入口为 `tools/mosaico_bundle.py --project Devices/MosaicoNative --idf-path <SDK路径>`。

已有的最小移植实机验证记录不代表此目标已完成触控、录音、存储或性能验收。
