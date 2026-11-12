# ESP-Mosaico 最小移植实机验证

日期：2026-11-11。范围：系统调度、原始 UI 与 BSP 音频输出，不包含键盘、触控、NAND 应用存储或四轨满载验收。

## 结果

- 通过保留的 Vibe Mode 安装并运行 `CardenzaSynthMosaico 0.2.0`，官方 ESP-IDF master 为 `0e6b480f858c69862ffb6001d96e9eef74599a6e`。
- 用户现场确认看到了屏幕中央原始界面，并听到了启动短乐句。
- Iris 截图核对为 480×480，原始 240×135 画面位于 `(120,172)`，外围像素全黑；截图来自显示任务完成传输后的快照，不是面板读回。
- 最终诊断版的默认 20% 音量 Juno C–E–G 启动测试和 RPC 重放均通过；各产生 316 个非零音频块，写入失败为 0，悬挂音符为 0。
- 6 次状态采样覆盖 **108.963 秒**：系统帧增长 10,897（约 **100.006 Hz**），音频块增长 18,773，对应约 108.977 秒的 44.1 kHz / 256 帧音频。无新增音频写入失败、断流估计或音频渲染超时；记录到 1 次系统帧迟到。
- 采样期间内部 RAM 空闲保持 211,195 字节，PSRAM 空闲保持 14,730,184 字节；未发现本次观察期间持续增长的内存占用。
- 最后状态确认同一 Boot ID 连续运行超过 242 秒，`normal` / healthy，崩溃计数为 0。

## 身份与更新证据

| 项目 | 记录 |
|---|---|
| Device ID | `4553502d4952495301001c2904d088f4` |
| 硬件 MAC | `1c:29:04:d0:88:f4` |
| 初始固件 | Vibe Mode 0.3.0，Recovery ABI 2 |
| 初始 Vibe Mode Boot ID | `732373620683670390` |
| 首版应用 Boot ID | `18202751620779698804` |
| 最终诊断版更新的 Vibe Mode Boot ID | `2450758821046058088` |
| 最终应用 Boot ID | `15016202975941553237` |
| 最终应用 ELF 身份 | `07862abe53b034745ad187e76f7f59ec031244c6560b9939950ff472e5021bc3` |
| 最终应用 BIN SHA-256 | `31c833c142051790027efba65ade5f6556db7820ce7ba8fd7cffa5c506805866` |
| 最终应用 BIN 大小 | 1,990,560 字节 |
| 完整分区表 SHA-256 | `8d28cdded305dd5f31c4a2aec315e4577e8e55f8ade887775d543306e6eb59a6` |
| Bootloader SHA-256（更新前后保持） | `3c1bdd7d58cd49ece060027f666a5f557564ea799ccda8822813b7dfec38598c` |

首版安装操作：`1a482079-4af5-45a1-87eb-cb76f8dca0af`。诊断版安装操作：`45642b34-9ee5-476a-8a61-e51910dc951a`。两次操作均为 succeeded，身份与固件摘要校验通过。

初始发现中存在已失效 OP-Ay1 Gateway 的旧归属，通过产品 CLI reconcile 清理后再连接；没有停止其他活跃 Gateway。更新前保存了旧固件日志和既有有效 core dump。旧 dump 的原因为 `UI task stalled`，缺少匹配旧 ELF，未归因于本次固件。

`app-update` 因本机缺少它期待的基础固件登记而在写入前拒绝执行；改用 `system-update`，保留既有 Vibe Mode 与 bootloader，仅写入相同分区表和目标应用，没有重刷基础固件、格式化 NAND 或擦除用户数据。

## 新增的测试接口

Iris 服务 `0x4343`：方法 1 返回只读运行计数，方法 2 排队一次短乐句。RPC 线程不直接操作音序器，实际测试由系统任务执行。Iris 截图后端只读，不提供键盘或触控输入。

截图实现的主机替身测试通过，覆盖完整像素映射、奇数分块、快照一致性、超时、取消与资源释放；诊断版 S31 增量构建通过。前一阶段共享代码的 47/47 主机测试仍为对应共享实现的回归证据。

## 边界

本次是基础启动、输出与短时稳定性验证。尚未验证键盘外设、录音、文件持久化、低内存降级、长期满载或全部音源复音性能。启动过程有 4 次渲染超过单块时长，被输出缓冲覆盖；采样观察期间该计数未增加。`estimated_underruns` 是队列时间估算，不是硬件 DMA 中断计数。FreeRTOS runtime stats 未启用，未计算整机 CPU 占用率。

通用 `iris memory` 的任务内存查询在该配置下返回 `not_supported`；本次内存结论来自应用公开的 heap capability 计数和 Iris 实时状态，不代表已经获得每个任务的内存分解。

## 证据位置

- [设备截图](../outputs/mosaico/device-20261111/original-ui.png)
- [稳定性采样与检查](../outputs/mosaico/device-20261111/stability-summary.json)
- [完整六次采样](../outputs/mosaico/device-20261111/stability-samples.json)
- [最终身份状态](../outputs/mosaico/device-20261111/final-status.json)
- [最终更新操作](../outputs/mosaico/device-20261111/diagnostic-system-update.json)
- [原始启动日志](../outputs/mosaico/device-20261111/final-app-logs.jsonl)
- [启动关键行](../outputs/mosaico/device-20261111/startup-lines.txt)
- [产物版本和验证清单](../outputs/mosaico/versions.json)

证据与 BIN / ELF / map / `.irisfw` 在 `outputs/mosaico/`；该目录为本地构建与验证产物，未纳入 Git。
