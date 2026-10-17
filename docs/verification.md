# 验证状态

本地已验证：

- GIBox 模型格式：正常读取（部件、骨骼、款式、贴图、`diffuseOnly`、按名字导入），以及 28 种错误各自被拒绝（其中 26 种核对了报出的原因：格式名、版本、角色 ID / 无 ID 无名字 / 名字格式、CRC、大小、对齐、长度、越界、索引越界、索引数、NaN、权重和、负权重、骨骼序号、绘制范围、未知材质、槽位、组合数量、款式漏赋值 / 未声明变量 / 默认组合、部件名、骨骼名数量、target；另有 `model.bin` 截断和缺失）；`learned.json`、`skeleton.json` 写出再读回；CRC-32 标准值。JSON 与 ELF note、DDS BC1 边界。
- 原生运行时模拟：同一角色两个 MOD 以先开启的为准（后开启的先建好、顺序对调两种情况）、被完全覆盖的 MOD 不构建、骨骼库在角色加载中不报错且失败原因只报一次；按特征匹配、详情页与资源重载跟随、NPC / 骨骼顺序守卫、多模型同时生效、闲置缓存、热重载与坏版本回退、款式原地切换、骨骼库导出、失败锁定、挂载失败回滚、GC 句柄释放；后台线程模式冒烟测试。交出模型时同步挂上与同步构建（模型文件已预载）、游戏换回 Mesh 后放回（交出回调、帧间 LOD）、游戏绑定 Mesh 时换参数（LOD、未见过的 LOD、放行条件、线程）、丢掉挂载时放回原材质、非 GameObject 忽略、关闭后不再处理、复制出的实例接管与还原、源渲染器消失后材质副本交接、版本更新后部件跟随新 Mesh（以及换皮肤时报告而不猜）。类型定位用非标准布局的假类测试：byval_arg 与 this_arg 不相邻、类型枚举非标准、byref 位为 0x20，带两种诱饵；分别只给 this_arg 配对、只给数组类指针、两者都没有（扫描关闭、只导出一次 dump、状态说明原因）。
- 运行时模拟通过 ThreadSanitizer 与 AddressSanitizer / UndefinedBehaviorSanitizer。当前宿主不支持 LeakSanitizer。
- MOD 重命名（只存显示名，留空恢复、坏文件忽略）。
- Java 转换器：合并款式、条件流、顶点压缩、权重归一化、UV / 纹理翻转、target 复制、原子替换（同角色保留 `learned.json` / `bonemap`，换角色丢掉）、拒绝别的角色的骨骼记录、失败目录回滚、严格 JSON；产物再由 C++ 读取器按运行时规则整份读取。
- 0.5 → 0.6 迁移：旧 Mods 移入私有目录（重名另起名、保留 `modelswap.txt`、清掉 `.pending`）、`control.json` 只在新目录没有时沿用、迁移后不再重复；读不了的 `model.json` 在列表里标“需重新转换”。
- GIMI 常见写法与按键切换 MOD；手动导入：角色表与 7.1 列表逐条一致、按 ID / 名称查找、推荐角色（含连写名、旅行者打平、只在骨骼库里的角色）、文件夹复制与 zip 解压、zip 越界拒绝、骨骼库转换与按 ID 转换、换角色后不沿用旧部件、Mods 列表、删除；XXMI 日志行格式与级别。
- 运行时：按 ID + 模型名导入的模型在角色不在场时从可操作角色骨架识别部件（NPC 同名不算）、保持 ID；换角色重新导入后忘掉旧部件；日志行格式、实例号、级别过滤。
- Java 全部应用源码通过宿主 API 签名编译；源码审计通过。

**0.2.0 基线已使用上表所列工具版本完成实际 Android APK 编译：`:app:assembleDebug` 返回 `BUILD SUCCESSFUL`。** 包名为 `dev.modelswap.only`，基线版本 `0.2.0-source`，最低 Android 8.0（API 26），仅包含 `lib/arm64-v8a/libmodelonly.so`。APK v2 调试签名校验、ZIP 对齐检查、ARM64 ELF 与 JNI 导出检查通过；ELF LOAD 段为 16 KiB 对齐。修正依赖后，源码检查和宿主测试再次通过。

**0.2.1 本次修复已通过 Java 源码宿主编译、全部宿主测试以及转换输出的 C++ 解析验证；没有重新执行实际 Android APK 构建。** 构建配置保留 0.2.0 已修正的 Dobby 固定版本、目标限制及 build-tools 版本。重新构建安装后，界面应显示 0.2.1，以确认测试的是新版转换器。

**0.6.x：同样没有执行 Android APK 构建、没有实机运行。** 宿主测试、ASan/UBSan、TSan、界面编译检查都通过；新格式和目录迁移需要实机确认（看 `GIBox Log.txt` 里的 `model-error`、状态卡片里的“游戏端”一行）。

**0.5.0：没有执行 Android APK 构建，也没有在手机上看过新界面**（这里拿不到 Google Maven）。做过的检查：Java 源码对真实的 `android.jar`（API 37）编译通过；Kotlin 界面对按 Material 3 `1.5.0-beta01` 的 API 签名文件、Compose-Settings 3.3.0 源码签名生成的桩编译通过，并带上 Compose 编译器插件跑了它的前端检查（@Composable 只能在 Composable 里调用等）。这些检查能发现参数名、类型、缺少 opt-in、Composable 调用位置这类错误，发现不了运行时的显示效果。

**0.3.6 之后的定位：** 断点采样确认游戏经原生 LOD 切换（`0x5798010`）和 `BindMeshBones`（`0x56e4d80`）写回原 Mesh。0.3.7 在这两个入口换参数，**尚未在实机确认**；看日志 `bind-substituted`、`bind-incompatible`、`hook-skip`。

**0.3.5 实机反馈：** 交出时挂上了，但游戏回调随后把 Mesh 设回、大世界还会按 LOD 换 Mesh，所以三个页面都先闪原模型；关闭替换后大世界变紫（丢掉挂载时销毁了渲染器仍在用的材质副本）。0.3.6 针对这两点修改，**尚未在实机确认**。

**0.3.4 交接诊断（handoff-20377）：** 关闭替换时采到 7 次角色交出、5 个角色：交出时模型根还没激活，渲染器上已有 Mesh、材质、骨骼，与 Tick 同一线程，交出后到激活前后引用没有再变。0.3.5 据此在交出时替换，**尚未在实机确认**；日志 `spawn <根名>: attached N` 表示在交出时挂上，`spawn-overwritten` 表示游戏随后又改了。这些样本都是界面用的模型（根名带 `Manekin`），大世界角色是否也走这一步还没确认，所以扫描仍保留作兜底。0.3.4 的模板与对象池预替换已撤掉（每 2 秒的全量扫描开销大，也没在实机验证过）。

**0.3.x 实机反馈（0.3.1 日志）：** 一键导入识别角色、骨骼库记录、当前操控角色的模型构建与挂载均已生效（`attached Mod/Part0 -> Body`）；全场景扫描失败（`SkinnedMeshRenderer Il2CppType not found`），因此组队界面、详情页没有替换。0.3.3 改为用 this_arg 配对与数组类两种独立特征定位 `byval_arg`，**尚未在实机确认**；日志 `renderer-type` 会写出找到的偏移和依据，失败时 `renderer-type-dump` 写出三个类及数组类的前 0x180 字节。

0.3.3 没有重新执行 Android APK 构建。仍需实机确认的点：`FindObjectsOfType` 返回值、原 Mesh 的 `+0x74/+0xc0/+0xe0` 在不可读资源上是否保持、子网格原地改写后 SkinnedMeshRenderer 的表现、游戏进程内转换大贴图的内存占用。

**尚未进行 Android 实机安装、LSPosed 加载或新版模型外观测试。** 编译与静态校验通过也不代表已验证游戏运行效果。

宿主测试可执行：

```bash
python3 tools/check_source.py
python3 tools/test_host.py
```

需要 Python 3、g++（C++17）、JDK 17。`tools/host_stubs.py` 只提供宿主检查的 API 签名，正式 APK 由 Android SDK / Xposed API 编译，不包含这些 stub。
