# GIBox

![license](https://img.shields.io/badge/license-MIT-blue)
![android](https://img.shields.io/badge/Android-8.0%2B-3DDC84)
![abi](https://img.shields.io/badge/ABI-arm64--v8a-informational)
![lsposed](https://img.shields.io/badge/LSPosed-module-orange)

---

GIBox 是一个 Android（LSPosed）模块：把 PC 上的原神角色 MOD（GIMI / XXMI 格式）转换成自己的模型格式，进游戏后替换对应角色的模型。界面用 Jetpack Compose（Material 3 Expressive），转换器是 Java，运行时是 C++。

当前版本 **0.6.2**，包名 `dev.modelswap.only`。

## 已实现

- **导入**：选 MOD 的 `.zip` 压缩包，选角色（按名字推荐），转换，启用
- **转换器**：INI 条件、Resource、CommandList、`drawindexed`、顶点解压、权重归一化、UV / 纹理翻转、DDS（BC1–BC7）解码、材质；按键款式合并进同一个模型
- **替换**：按原 Mesh 的特征识别渲染器，场景、详情页、组队界面、联机里的同一角色都会换，NPC 不受影响；原资源不修改
- **骨骼库**：角色出场时自动记录骨骼，转换时按骨骼名对应
- **多 MOD**：同一角色可同时开多个，同一部件以先开启的为准
- **热更新**：模型目录变了在后台重新载入，坏版本不会把画面上的模型撤掉
- **界面**：MOD 按角色分组，可重命名；款式在 App 里切换；XXMI 样式的日志，级别可调
- **模型格式**：GIBox 自己的 `model.json` + `model.bin`，带版本号、CRC-32 和整套范围检查，见 [docs/model-format.md](docs/model-format.md)

## 使用方法

### 安装

1. 自己编译 APK（见下面“编译”）并安装
2. LSPosed 里启用 GIBox，作用域勾选**游戏**和 **GIBox 自己**（勾上自己，主页才显示“模块已激活”）
3. 重新打开 GIBox，彻底重启游戏
4. 设置 → 通用：选服务器（官服 / B 服），授予“所有文件访问权限”（转换好的模型要放进原神的媒体目录，游戏才读得到）

### 导入 MOD

1. 主页点“导入 MOD（.zip）”，在系统文件管理器里选压缩包。压缩包里多一层文件夹没关系，一个压缩包只能有一个 MOD；rar / 7z 先重新压缩成 `.zip`
2. 在角色页选角色：搜索框可输入 ID、中文名、英文名或模型名；从 MOD 名字猜出的角色排第一，标“（推荐）”；点“确定”才开始导入，返回则什么都不导入
3. 转换（大 MOD 可能要一分钟），完成后自动启用
4. 主页按角色列出 MOD，右边是数量；点角色看它的 MOD，开关直接启用 / 停用；再点进 MOD 详情：重命名、更换角色并重新转换、款式、转换记录、删除

同一角色打开第二个 MOD 时会弹窗：**都开启**（同一部件以先开启的为准）/ **只开这个** / **取消**。

角色在大世界出场时运行时会记录骨骼库（不需要开启替换）；之后对这个角色“重新转换”就会走骨骼库，进游戏任何页面都直接替换。没有骨骼库时按游戏原骨骼顺序转换，角色出现在大世界、队伍界面或详情页时识别部件。

### 设置

| 项 | 内容 |
|---|---|
| 通用 | 服务器、文件访问权限 |
| 导入器 | 日志详细程度：禁用 / 警告 / 信息 / 调试，写进 `control.json` 的 `logLevel`，游戏和 App 都按它过滤；查看日志 |
| 高级 | 导出当前角色骨骼；手动转换（填 MOD 文件夹、骨骼目录、角色 ID、模型名和可选的 `部件=渲染器;部件=skip` 映射） |
| 关于 | Mods 文件夹、模型目录、骨骼库、日志文件、Build ID，点一项复制 |

## 支持的游戏版本

目前只适配这个 GNU Build ID：

```text
2b4dd73155e845a2ef698ec500509baa4eb3bfa8
```

运行时读取已加载的 `libyuanshen.so` 的 GNU note。Build ID 不匹配时不会安装 hook，也不会调用地址表里的游戏函数，模块直接不启动。

### 游戏版本更新后

- **引擎地址**（`offsets.hpp`）：每次更新 `libyuanshen.so` 都会变。每个版本必须重新做的只有这一件：重新定位地址表里的函数，改 Build ID，重新编译
- **骨骼库**：每次进游戏、角色第一次出现时重新记录，自动是新版本的数据
- **已导入的模型**：用骨骼库转换的按骨骼名对应，官方只调整骨骼顺序时照常可用。官方改了角色模型本身时：同一个渲染器、同名 Mesh、骨骼数不变（只是顶点数 / 子网格变了），部件自动跟到新 Mesh（日志 `target-updated`）；Mesh 名变了（例如换皮肤）或骨骼数变了，不做猜测，状态里写明需要对应的 MOD 重新导入——和 PC 上更新后等作者更新是同一回事

## 工作原理

1. 运行时（`runtime.cpp`）用 `Object.FindObjectsOfType(SkinnedMeshRenderer)` 找到所有激活的渲染器，按原 Mesh 的特征（名字、顶点数、子网格数、bindpose 数）识别部件
2. 命中后克隆 Mesh，用 Mesh API 填入转换好的顶点、索引、权重，`setMesh` / `setMaterials` 挂上；骨骼按名字唯一对应并检查 bindpose 容差，异常时不挂载
3. 交出模型的回调、LOD 切换、`BindMeshBones` 三处用 hook 换参数，避免游戏随后把原 Mesh 设回去
4. Tick 每 100 ms 处理一次，每次最多构建一个部件；文件读写全部在后台线程

PC 上的 MOD 没有角色 ID，只有 D3D11 缓冲区的 hash，加载器在 GPU 画到这个 hash 时替换。手机版缓冲区与 PC 不同，hash 不能直接用，这里用 Mesh 特征代替 hash，MOD 对应哪个角色由名字识别。

## 文件位置

`<GIBox>` = `/storage/emulated/0/Android/media/<游戏包名>/GIBox`

| 内容 | 位置 |
|---|---|
| Mods（导入时复制 / 解压到这里，只有 App 读） | GIBox 私有目录 `files/mods/<游戏包名>/<名称>/` |
| 模型 | `<GIBox>/models/<模型名>/`：`model.json`、`model.bin`、`conversion.log`；游戏写 `learned.json`、`bonemap-<部件>.tsv` |
| 骨骼库（自动） | `<GIBox>/skeletons/<角色ID>/skeleton.json` |
| 控制 / 状态 / 日志 | `<GIBox>/control.json`、`status.json`、`GIBox Log.txt`（超过 2 MB 转为 `.txt.1`） |
| 启动失败记录 | 游戏私有 `files/gibox/bootstrap-error.txt` |

游戏进程打不开别的 App 的私有目录，所以游戏要读写的放在媒体目录。

## 编译

| 工具 | 版本 |
|---|---|
| JDK | 21（17 以上） |
| Gradle | 9.7.1 |
| Android Gradle Plugin | 9.4.1 |
| Kotlin / Compose 编译器插件 | 2.4.20 |
| Android SDK | compileSdk 37.1，build-tools 35.0.0 |
| Android NDK | 27.2.12479018 |
| CMake | 3.22.1 |
| ABI | arm64-v8a |

用最新的 Android Studio 打开本目录即可，缺的 SDK 平台会提示安装。命令行：

```bash
gradle --no-daemon :app:assembleDebug
```

产物：`app/build/outputs/apk/debug/app-debug.apk`。首次构建需要联网下载 Gradle 插件、Compose / Material 3、Compose-Settings、Xposed API，以及固定版本的 Dobby 源码（`CMakeLists.txt` 拉取并编译成静态库，链接进 `libgibox.so`）。bcdec 已包含在 `third_party/bcdec`。

仓库带 GitHub Actions（`.github/workflows/build.yml`）：推送后先跑源码检查和宿主测试，再构建 APK 并上传为构建产物。

宿主测试（不需要 Android SDK）：

```bash
python3 tools/check_source.py
python3 tools/test_host.py
```

需要 Python 3、g++（C++17）、JDK 17。验证了什么、没验证什么见 [docs/verification.md](docs/verification.md)。

## 项目结构

| 位置 | 内容 |
|---|---|
| `app/src/main/java/dev/modelswap/only/` | App 与转换器 |
| `app/src/main/cpp/` | 游戏内运行时 |
| `tests/` | 宿主测试（Java / C++） |
| `tools/` | 检查与测试脚本 |
| `third_party/` | bcdec、Dobby 头文件与许可证 |
| `docs/` | 模型格式、验证状态、诊断说明 |

主要文件：

| 文件 | 职责 |
|---|---|
| `Entry.java` / `NativeBridge.java` | LSPosed 入口、加载 `libgibox.so`、JNI |
| `MainActivity.kt` / `AppState.kt` / `Theme.kt` | Compose 入口、页面返回栈与全部操作、主题与对话框 |
| `HomeScreen.kt` / `CharacterScreen.kt` / `DetailScreen.kt` / `PickScreen.kt` | 主页、角色的 MOD 列表、MOD 详情、选角色 |
| `SettingsScreen.kt` / `LogScreen.kt` / `ManualScreen.kt` | 设置、日志、手动转换 |
| `Importer.java` / `SafSource.kt` / `Characters.java` / `Labels.java` | 导入（zip 解压）、角色表与推荐、MOD 显示名 |
| `ModelConverter.java` | 转换器：合并款式，写 `model.json` + `model.bin`，原子替换 |
| `ModelManifest.java` / `Variants.java` / `Json.java` | 读清单、款式变量、严格 JSON |
| `ModLibrary.java` / `Control.java` / `Migration.java` | 骨骼库与 Mods 目录、控制与状态文件、旧版本目录迁移 |
| `bootstrap.cpp` | 检查 Build ID、安装 Tick hook、DDS JNI |
| `runtime.cpp` | 识别、构建、挂载、恢复、热重载、款式、骨骼库 |
| `model.hpp` | 模型格式读取与全部检查、CRC-32、`learned.json`、`skeleton.json` |
| `core.hpp` / `json.hpp` / `dds.cpp` | 对骨与 Mesh 特征、JSON、DDS 解析 |
| `offsets.hpp` | 仅此 Build ID 的游戏地址表 |

## 当前限制

- **没有实机验证**：0.2.0 完成过 Android APK 构建；0.3 到 0.6 的改动只通过宿主测试、ASan / UBSan / TSan 和界面编译检查，没有执行 APK 构建，也没有在手机上运行。安装后先看状态卡片里“游戏端”一行和 `GIBox Log.txt`
- **权重缓冲区**：每次构建向引擎分配器申请一块新的权重缓冲区，克隆 Mesh 原来那块不释放（没有确认对应的释放函数），每次切换模型留下一块 32×顶点数 字节，正常使用影响很小
- **不能转换**：替换 shader 的 MOD（`ps` / `vs` override、靠 shader 实现的发光或描边修正、按帧切换贴图）依赖 D3D11 管线；数组 / 立方体 DDS 会明确报错
- **只适配一个游戏版本**，见上面“支持的游戏版本”

## 风险提示

模块通过 Dobby inline hook 改写游戏函数入口，并直接读写引擎内存。地址表与游戏版本不一致、或模型数据异常都可能让游戏闪退。仅供学习和研究，请勿用于非法用途，使用风险自负；本项目与游戏厂商无关。

## 许可证

业务代码按 [LICENSE](LICENSE)（MIT）提供；第三方代码保留各自许可证，见 [THIRD_PARTY.md](THIRD_PARTY.md)。更新记录见 [CHANGELOG.md](CHANGELOG.md)。

## 鸣谢

- [XXMI-Launcher](https://github.com/SpectrumQT/XXMI-Launcher)：日志格式与等级、界面风格和中文用词的参考
- [GI-Model-Importer（GIMI）](https://github.com/SilentNightSound/GI-Model-Importer)：转换器读取的 MOD 格式和 PC 上的模型替换方式
- [3DMigoto](https://github.com/bo3b/3Dmigoto)：GIMI / XXMI 所基于的 D3D11 注入框架，MOD 的 INI 语法出自它
- [Dobby](https://github.com/jmpews/Dobby)、[bcdec](https://github.com/iOrange/bcdec)、[Compose-Settings](https://github.com/alorma/Compose-Settings)、[LSPosed](https://github.com/LSPosed/LSPosed)：hook、BC 纹理解码、设置界面组件、模块运行环境
