# CardenzaSynth

运行在 Cardenza 上的四轨 groovebox，配备 240×135 屏幕和 4×14 带灯键盘。

- DX7、Juno、Sampler、Drums 四种音源，每轨独立和弦与琶音器（鼓轨除外）。
- 每个 Pattern 1–256 步，支持实时录音、Pattern 切换和整曲编排。
- 四轨混音、轨道效果、延迟与混响发送及总线效果。
- 工程、音色、采样和自动保存均存于 SD 卡。

[教程与手册](wiki/README.md) · [浏览器模拟器](https://cardenza-synth.203.io) · [界面演示](docs/groovebox/motion)

## 基本操作

中间 4×8 的 PADS 演奏当前轨，右侧 4×4 的 GRID 对应屏幕格子。

| 键 | 单击 | 按住 |
|---|---|---|
| ESC | 乐器 → 音序器 → 走带；取消 / 返回 | GRID 选择场景，第一列进入 SETUP |
| TAB | 下一轨 | GRID 选轨、静音、独奏与音序开关 |
| FN | 当前页面菜单 | GRID 执行菜单项或使用快捷键 |
| CTRL | 下一页 | GRID 直接选页 |
| ADD / SUB | 调值或移动 | 连发；FN 组合为大步调整 |
| ENTER | 确认、进入、切换 | SEQ 中打开录音面板 |
| PLAY | 播放 / 停止 | FN + PLAY 开关实时录音 |

在 SEQ 中，按住演奏键再点 step 写入音符；点 step 选中后，用 PADS 加减音符、ENTER 切换编辑项、ADD / SUB 调整。FN + S 保存工程，FN + ESC 全部停止。

开机没插 SD 卡时，本次运行不保存数据；插卡后需重启。数据目录为 `CardenzaSynth/`，详见[存储协议](docs/app-storage-protocol.md)。

## 构建

先初始化子模块：

```sh
git submodule update --init --recursive
```

固件需要 ESP-IDF 5.3.5，在已加载 ESP-IDF 环境的终端运行：

```sh
make DEVICE=Cardenza build
```

主机模拟器与测试：

```sh
cmake -S . -B build/host -DDEVICE=CardenzaSim -DBUILD_TESTING=ON
cmake --build build/host --config Debug
ctest --test-dir build/host -C Debug --output-on-failure
```

浏览器模拟器需要 Emscripten SDK：

```sh
make web
make web-serve
```

- [固件构建与烧录](Devices/Cardenza/README.md)
- [桌面模拟器、截图与测试](Devices/CardenzaSim/README.md)
- [浏览器模拟器](Devices/CardenzaWeb/README.md)
- [ESP-Mosaico 最小移植](Devices/Mosaico/README.md)
- [ESP-Mosaico 原生界面与业务分层](Devices/MosaicoNative/README.md)
- [开发规则与设计](AGENT.md)
- [Demo 生成工具](tools/demo/README.md)
