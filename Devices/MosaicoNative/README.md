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

## 工具入口

从仓库根目录使用 `mosaico.py`，沿用[最小移植的依赖与 SDK 配置](../Mosaico/README.md)，
将项目参数设为 `--project Devices/MosaicoNative`。
`mosaico.py native-files --help` 和 `mosaico.py native-touch --help` 分别说明文件与触控工具。
离线打包入口为 `tools/mosaico_bundle.py --project Devices/MosaicoNative --idf-path <SDK路径>`。

已有的最小移植实机验证记录不代表此目标已完成触控、录音、存储或性能验收。
