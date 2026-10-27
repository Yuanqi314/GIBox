# 3DMigoto（XXMI 分支）来源记录

`app/src/main/cpp/migoto/` 里的 INI 命令列表逻辑核心，是按下面这个上游版本的源码移植的衍生代码，随 GIBox 组合版本按 GPLv3 发布（见仓库根目录 `LICENSE`、`COPYING`）。

## 上游与固定版本

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/SpectrumQT/XXMI-Libs-Package（XXMI DLL，3Dmigoto 的分支，XXMI Project / AGMG 社区） |
| 原始项目 | https://github.com/bo3b/3Dmigoto |
| 固定提交 | `a7847eae82aaac32175c9a7d199673d5c9323bcd`（SpectrumQT，2026-10-06） |
| 取得方式 | `git clone --depth 1`，只读参考，没有把上游文件放进本仓库 |

## 授权核实

- 上游 `COPYING.txt`：3Dmigoto 源码按 GPLv3 发布（原文已复制为本目录 `COPYING.txt`，内容未改）。
- 上游 `LICENSE.GPL.txt` 是 GPLv3 标准全文，SHA-256 `8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903`，与仓库根目录 `COPYING` 逐字节相同。
- 参考过的源文件（下表）都没有单独的版权或许可证头，所以适用 `COPYING.txt` 的 GPLv3。
- 上游 `COPYING.txt` 提到的另外两类内容都没有引入：Nektra Deviare-InProcess 库（GPLv3）没用到；随游戏修复分发的着色器不在 GPL 范围内、归各自所有者，也没用到。
- 作者：上游 `AUTHORS.txt` 复制为本目录 `AUTHORS.txt`，只把编码从 Latin-1 转成 UTF-8，内容未改。XXMI 分支的修改归其贡献者（见上游 git 历史）。版权仍归原作者，本仓库没有改动或移除任何上游声明。

## 引入方式和对应关系

没有逐文件拷贝上游代码。做法是读上游函数，用不依赖 DirectX、Windows、Unity、JNI 或 hook 框架的 C++17 按同样的语义重写；结构和判断顺序跟着上游走，所以按衍生作品处理。

| GIBox 文件 | 上游文件（固定提交） | 移植的部分 |
|---|---|---|
| `migoto/ini.hpp` | `DirectX11/IniHandler.cpp` | `ParseIniStream`、`ParseIniSectionLine`、`ParseIniKeyValLine`、`ParseIniPreamble`（`namespace =`、`condition =`）、`get_namespaced_section_name`、`get_namespaced_var_name_lower`、CommandListSections / RegularSections 表 |
| `migoto/expr.hpp` | `DirectX11/CommandList.cpp`、`DirectX11/CommandList.h`、`util.h` / `util.cpp` | `tokenise`、`group_parenthesis`、`transform_operators_*` 及其在 `CommandListExpression::parse` 里的顺序、`DEFINE_OPERATOR` 运算符表、`CommandListSyntaxTree::finalise`、`ParseFloatToken`、`FindVariableTokenEnd` / `NamespaceScanner`、`valid_variable_name`、`ParseIniParamName`、`ParamOverrideTypeNames`、`operand_allowed_in_context`、`static_evaluate`、`random()` 的取值范围 |
| `migoto/engine.hpp` | `DirectX11/CommandList.cpp`、`DirectX11/IniHandler.cpp`、`DirectX11/Override.cpp`、`DirectX11/HackerDXGI.cpp`、`DirectX11/HackerContext.cpp`、`DirectX11/ResourceHash.cpp` | `ParseCommandList`、`ParseCommandListLine`、`AddCommandToList`（pre/post 放置）、局部变量与作用域、`if / elif / else if / else / endif` 的解析和 `IfCommand::run`、`RunExplicitCommandList`、`CheckTextureOverrideCommand`、`handling`、`VariableAssignment`、绘制命令的键、`_RunCommandList`（递归上限 256、abort）、`process_texture_filter`、`ParseConstantsSection` / `RegisterGlobalVariable`、TextureOverride 的设置键与排序（`TextureOverrideLess`）、`matches_draw_info`、`parse_fuzzy_numeric_match_expression`（数值形式）、按键类型、条件、cycle（wrap、smart）、hold 的全局保存、transition、按键的 run；`RunFrameActions` / `Present` 的执行顺序、`[Constants]` 的 pre/post、`BeforeDraw` / `AfterDraw` |
| `migoto/adapters.hpp` | 无上游代码 | 导入端静态扫描、游戏端会话（建立在上面的移植之上，所以同样 GPLv3） |

完整 SHA-256：

```
f6755f648133dc81796c22f09ca2ab2ec854df49fc169d515a81c8f76c2d1c2a  COPYING.txt
24d325a6aaef77e1b43436cab23a2cbb054f4fb325df679953c1886282271848  AUTHORS.txt
8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903  LICENSE.GPL.txt
ce07e1404d6668207a29f64067c294a274ac79ca3a32aee79a5eb0d69bc46aa3  DirectX11/CommandList.cpp
6493f28fd0f5ea082ad7f2ec8e20fb0b3207605b47b973e0f795ab9e658da834  DirectX11/CommandList.h
a17a6f85856d4a2337904664e0585f641ed30a12321ca7d1b0c09844a120054a  DirectX11/IniHandler.cpp
65779c521809967d499bc1f2d3fb4b1f5c315630b4cd5deca2364a8a94751e00  DirectX11/IniHandler.h
82d1719672d92e9180c2c1d9c96a86dd083158dedb5d00aebe759960db8f7434  DirectX11/Override.cpp
c47983f81fc9ac1df8cc5dce6bbc28212ef744516d63a56967285aba9137fe6c  DirectX11/HackerDXGI.cpp
2835728d34e2ae200a8a39fbbc965ad6cc8a0390dd947b4d8a8c31791b456615  DirectX11/HackerContext.cpp
8e6ef6d0b866bea034be7e3f4aa289eee95aa3dfbe9d4503f8267fa588616854  DirectX11/ResourceHash.cpp
ad5c22e00cf91ce7725ef3b7a3e83afa6a72c6dc7c0457782aba0ffa3422babe  util.h
87fec5583dd0b0ee663ddbc395ba72c0762f5c3190de577adf824a01bc4eb2df  util.cpp
```

## 修改记录（与上游的差异）

1. 去掉了全部 DirectX、Windows、日志、屏幕提示、性能统计和帧分析代码；`wchar_t` 换成 UTF-8 `std::string`，大小写转换只处理 ASCII（上游用 `towlower`）。
2. 没有移植、只记录或报告的部分：资源复制和其他 GPU 操作（只记录槽位和 `[Resource]` 之间的绑定关系）、CustomShader、ShaderOverride / ShaderRegex、Pool、Preset、`d3dx_user.ini` 的读写（由适配层提供保存和恢复）、按键的 `delay` / `release_delay`、无 hash 的资源描述匹配、`match_*` 的字段表达式、XXMI 的资源目标表达式（如 `ResourceX->Size`）、scissor、`dxgi_format_*` / `d3d11_bind_*` 常量、间接绘制、`CommandListX = ...` 复制、内置命令列表（`BuiltInCommandList*`）、命令列表优化（`optimise_command_lists`，只影响性能）。
3. 行为上的差异：
   - `random()`：用适配层给的均匀随机源，范围和 24 位精度相同；上游用帧号、调用计数和时钟做哈希，数值不可复现。
   - `fps` 是瞬时值；上游做了平滑。
   - `(int32_t)float` 按 MSVC x64 的 `cvttss2si` 显式实现（NaN 和越界都得 `INT_MIN`），移位次数取低 5 位，和 x86 指令一致。
   - 宿主 d3dx.ini 里各着色器的 `checktextureoverride` 顺序不在固定提交里，由适配层按绘制给出。
   - 一次绘制的宿主命令列表合成一个执行状态；上游每个着色器阶段一个，只影响 `handling = abort` 的作用范围。
   - 绘制后（post）阶段按槽位当时的内容重新匹配；槽位已经换成 MOD 资源时不再匹配。这一条是根据上游 MOD 资源不登记 hash 推断的，没有在 PC 上验证。
   - 在绘制以外（如 `[Present]`）设置的槽位不会保留到之后的绘制里，并作为限制报告。

## 发布要求

包含这部分代码的发布（APK 或其他二进制）必须同时提供或承诺提供完整的对应源码：发布所用提交的本仓库内容，包括 Gradle / CMake 构建脚本、`tools/`，以及 `THIRD_PARTY.md` 列出的第三方源码或构建时下载的确切版本（GPLv3 第 6 节）。
