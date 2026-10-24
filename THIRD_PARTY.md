# 第三方依赖

这些是通用开源依赖，来自官方源码或构建仓库。

| 依赖 | 来源与固定版本 | 用途 / 交付形式 |
|---|---|---|
| Dobby | https://github.com/jmpews/Dobby · commit `0932d69c320e786672361ab53825ba8f4245e9d3` | Tick instrumentation；CMake 下载完整源码并编译静态链接。Apache-2.0，许可证 `third_party/Dobby-LICENSE`。官方头文件另存 `third_party/dobby-api/dobby.h` 供宿主语法检查。该版本使用内建 ARM64 assembler，避免原固定版本的 Mach-O 汇编与 Linux 平台接口编译错误。 |
| bcdec | https://github.com/iOrange/bcdec · commit `80859ed3b7afb1c527a2a99d70c61457bea72d0c` · v0.985 | BC 压缩块解码；完整单头文件包含在 `third_party/bcdec/bcdec.h`，许可证在文件末尾（MIT / Unlicense 可选）。DDS 容器和 JNI 是本工程编写。 |
| Xposed API | `de.robv.android.xposed:api:82` · https://api.xposed.info/ | 仅 compileOnly；正式运行由 LSPosed 提供。 |
| Jetpack Compose / Material 3 / Activity | `androidx.compose.material3:material3:1.5.0-beta01`、`androidx.activity:activity-compose:1.13.0`、`androidx.compose.material:material-icons-core:1.7.8`（Google Maven） | App 界面（Material 3 Expressive）。Apache-2.0。由 Gradle 下载，不在源码 ZIP 中。 |
| Compose-Settings | https://github.com/alorma/Compose-Settings · `com.github.alorma.compose-settings:ui-tiles-expressive:3.3.0`（Maven Central） | 设置页和 MOD 详情页的设置行（SettingsGroup / SettingsSwitch / SettingsRadioButton / SettingsMenuLink）。MIT。由 Gradle 下载。 |
| Kotlin | Kotlin 2.4.20 与 Compose 编译器插件 | App 界面用 Kotlin 写；转换器、导入、游戏端仍是 Java / C++。 |
| Android SDK / NDK / Gradle / AGP | 工程与 README 中的指定版本 | 标准构建工具与系统 API，不包含在源码 ZIP 中。 |

