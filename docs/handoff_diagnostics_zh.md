# GIBox 0.3.4 模型交接诊断补丁

这是 GIBox 0.3.4 运行时的诊断补丁，转换器、Mesh、材质和骨架实现不变。补丁自身不替换 Mesh、不修改资源参数；正常替换功能仍受替换开关控制。它使用 Dobby 原生 inline hook，会改写函数入口代码，不能等同于被动读内存，也不能保证不会闪退。

仅支持 GNU Build ID `2b4dd73155e845a2ef698ec500509baa4eb3bfa8`。所有地址按本进程 `load_bias + RVA` 计算，不使用上次进程的绝对地址。新增 hook 和读取 API 共 20 项，安装前逐项核对 16 字节指纹；失败时关闭新增诊断。不要把实测不匹配的字节填回预期值来跳过校验。Tick 入口在已有快照中被 hook 过，含进程相关地址，因此没有把该快照作为 Tick 的跨进程指纹。

本补丁已合入源码。**0.3.5 起默认不编译**：它与替换功能挂同一个交出回调，而且要求关闭替换。需要再采集时，用 `-DMODELSWAP_HANDOFF_DIAGNOSTICS=1` 编译（例如在 `app/build.gradle` 的 `externalNativeBuild.cmake.cppFlags` 里加上），这时不安装替换用的交出钩子。合入时做了三处改动，见文末“合入时的改动”。

继续使用项目原有构建流程，新增实现为 header-only，无需改 CMake 源文件列表。`-DMODELSWAP_HANDOFF_DIAGNOSTICS=0` 可关闭新增 hook 和采样。不要热卸载模块：发生部分安装失败时保留透明转发，待进程重启清理。

## 采集步骤

1. 先关闭原模型替换开关，确认 control.json 已保存，再完整退出并重新启动游戏。此前 mode 2 扫描可能已改过预制体和池对象，单纯关开关无法证明内存恢复到未修改状态。日志 `contaminated=true` 时重新采集基线。
2. 进入大世界，让 Tick 从当前角色已有的 renderer 列表识别并固定 `SkinnedMeshRenderer` 的 System.Type。`GIBox Log.txt` 出现 `handoff-type SkinnedMeshRenderer Type ready` 后采集。没有 Type 时只记录 skip，不做全场景查找。
3. 打开角色详情或队伍页面、换 A/B、关闭重开，再尝试世界切人。操作可按任意顺序，无需倒计时。建议先测一到两个角色，以免用完有限采样预算。
4. 读取 `/storage/emulated/0/Android/media/<包名>/GIBox/handoff-<PID>.jsonl`。官服包名为 `com.miHoYo.Yuanshen`。不是之前 MCP 的 check.json。保留整个 JSONL，包含 meta、skip、错误和预算信息。记录每次操作是否只是复用世界中已存在的角色。
5. 可在电脑执行 `python3 tools/analyze_handoff.py handoff-<PID>.jsonl --out handoff-summary.json`，返回 JSONL 和汇总。

日志后台写入；采样端只构建有界记录后入队（只在交换队列的瞬间加锁），不在 hook 中读写文件。上限：实例化/异步交接快照 256 次（只计根下有 SkinnedMeshRenderer 的；没有的只计数 `empty_spawns`，不是 GameObject 的计 `foreign_roots`），实体就绪/激活快照另计 256 次，每根 32 个 SMR，2048 次全局 Mesh setter 调用，队列 4096 条；材质记录前 16 个元素并标注截断，骨骼只记录数量与小样本。`dropped`、`write_errors` 或预算耗尽意味着日志不完整。文件不可写时没有 JSONL，应同时检查原 GIBox Log.txt 的启动错误。采样会造成额外开销，采完关闭诊断并重启。

每条记录含 `capture_start_ns`；若系统复用 PID 导致同名文件追加了多次运行，分析脚本只汇总最后一条 meta 对应的采集，不跨进程混合事件序号。

## 观察点与能得出的结论

| 观察点 | RVA | 记录时机 |
|---|---|---|
| `LoadedBundle.Instanite`（原名拼写） | `0x8f95b18` | 同步游戏实例化函数返回后，检查返回 GO 的 SMR/Mesh |
| `JDIIHHFJKBM.AAKKKEMMMCA` | `0x88770e0` | 异步资源接收成功、原接收函数执行前 |
| `JDIIHHFJKBM.KEKFNMKCGNO` | `0x8721e24` | 实体就绪函数前后 |
| `JDIIHHFJKBM.IFICFCFENMM` | `0x8713ec4` | 包含 `SetActive(true)` 的函数前后；入口并非紧邻 SetActive 的那条指令 |
| `SkinnedMeshRenderer.set_sharedMesh` native body | `0x49fc24c` | 参数与正常返回；原参数和返回流程保持不变 |

每个快照包含真实 `Time.frameCount`、独立事件序号、线程 ID、Tick 线程 ID、根和 renderer 的托管/原生身份、活跃状态、Mesh、材质元素和骨骼概况。setter 还记录单层原始返回地址 `caller`，可结合 meta.base 换算模块内调用点，不进行栈扫描。`tick_count` 是回调次数，不能当帧号。JSONL 行在并发入队时可能顺序交错，分析脚本按独立序号排序；序号标识采样开始顺序，并不把整个快照变成原子快照。

仅当当前线程等于实际观测的 Tick 线程、Tick 未出现多个线程、控制开关明确关闭且 Type 已就绪时调用 Unity 读取 API。其它线程只记原始 setter 参数/skip。不会把 tid=10410 或进程主线程号硬编码成主线程。原 Tick 100ms 节流不变；诊断 getter 内的嵌套 Tick 回调不调用 Runtime，防止重入。诊断 hook 不调用 Runtime::Impl。对象和临时托管数组在调用期间用 GC handle 固定，不销毁任何游戏对象。

GIBox 经 `Engine::set(setMesh/setMaterials)` 的赋值会通过 TLS 标记；setter 日志中 `source=modelswap` 与 `external` 可分开。`external` 只表示不是这些已标记路径，不保证一定来自游戏；其它插件也可能调用。材质比较使用元素而非 getter 新建的数组地址。

- 同步返回或异步交接时 SMR 已有 Mesh：说明在**该边界**前 Mesh 已存在，可优先验证在该边界替换；还不能单凭这一条认定 Mesh 必然直接来自预制体。
- 同一 renderer 之后出现正常返回的外部 setter，且后续快照 Mesh 对应：支持后续赋值路径。仍需按角色/部件过滤，不能全局替换所有 setter 参数。
- 未观察到 setter：不能证明游戏从不调用，可能在安装前、截断范围外、其它 native 路径或复用池中完成。
- 激活入口时 inactive、出口时 active：证明这次激活前存在介入机会。即使同一帧，也**不证明此前从未绘制**。报告始终保留 `first_draw_proven=false`；本补丁没有渲染提交/可见性边界证据。
- 世界切人无创建事件：可能仅复用已有对象。该情形应检查角色首次进入场景/入池的时机，并在首次替换后保留对象映射，不能据此宣布模型创建 hook 失效。

实体指针会复用；按一次交接周期和根/renderer 的原生身份看日志。同步 `assetPathHash` 为 32 位，与异步 64 位 `resource_key` 不是可直接等同的标识。当前诊断覆盖已定位的同步/异步交接分支，池命中或其它加载入口仍可能绕过它们。

## 验证范围

补丁按用户提供的 0.3.4 摘录生成；已核对新增 20 项指纹与提供的模块快照一致，并检查补丁在对应摘录中的应用及反向应用。宿主 C++17 mocks 覆盖指纹/日志初始化失败阻止安装、部分安装透明转发、原函数参数/返回值/异常、线程/开关门控、TLS 异常退出和 GC handle 配对，bootstrap 的诊断开/关配置均通过宿主语法检查。可用 `python3 tools/test_handoff.py` 重跑 mocks。

完整 0.3.4 项目、Android SDK/NDK 构建和真机运行尚未验证。这些宿主检查不能替代 ARM64 ABI、Dobby 并发安装和设备稳定性验证；AddressSanitizer/UndefinedBehaviorSanitizer 的宿主正常路径检查通过，本环境 LeakSanitizer 无法运行，未据此声称完成泄漏检测。

## 合入时的改动（相对原补丁）

1. **只对 GameObject 调用 GameObject 接口。** 异步接收函数的 `asset` 参数（以及理论上 `entity+0x278`）不保证是 GameObject，对别的类型调用 `GetComponentsInChildren`、`activeSelf` 等会读错原生对象。现在播种 Type 时顺便从世界角色渲染器的 `gameObject` 取得 GameObject 的 Il2CppClass，只有类指针完全一致的根才会被调用这些接口；不一致的记 `root_is_game_object=false`、`root_class`，并计入 `foreign_roots`。`get_gameObject` 的指纹已在原补丁的 20 项里。
2. **预算分开，并跳过无关实例化。** `LoadedBundle.Instanite` 和异步接收对特效等所有资源都会触发，原来 192 次的共用预算在大世界里可能很快用完。现在根下没有 SkinnedMeshRenderer 的实例化只计数，不占预算；实例化/交接与就绪/激活各 256 次。
3. **入队改为加锁等待。** 原来用 try_lock，写线程交换队列的瞬间恰好有快照入队就会被丢掉（宿主测试里能复现）。写线程只在交换时持锁，等待时不持锁，所以加锁只耗微秒。队列上限改为 4096 条。

测试相应增加：GameObject 类未播种时不调用、非 GameObject 的接收资源不调用、没有蒙皮渲染器的实例化不产生快照；并在 AddressSanitizer/UBSan、ThreadSanitizer 下运行。
