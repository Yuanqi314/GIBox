# GIBox

![license](https://img.shields.io/badge/license-MIT-blue)
![android](https://img.shields.io/badge/Android-8.0%2B-3DDC84)
![abi](https://img.shields.io/badge/ABI-arm64--v8a-informational)
![lsposed](https://img.shields.io/badge/LSPosed-module-orange)

---

GIBox 是一个把 PC 的原神角色 MOD 加载在 Android 版本原神上的 xposed 模块

### 导入 MOD

1. 主页点“导入 MOD（.zip）”，在系统文件管理器里选压缩包，一个压缩包只能有一个 MOD。
2. 在角色页选角色：搜索框可输入 ID、中文名、英文名或模型名；从 MOD 名字猜出的角色排第一，标“（推荐）”；点“确定”才开始导入，返回则什么都不导入
3. 转换，完成后自动启用
4. 主页按角色列出 MOD，右边是数量；点角色看它的 MOD，开关直接控制启停；再点进 MOD 详情：重命名、更换角色并重新转换、款式、转换记录、删除

角色在大世界出场时运行时会记录骨骼库（不需要开启替换）；之后对这个角色“重新转换”就会匹配骨骼库，进游戏任何页面都直接替换。没有骨骼库时按游戏原骨骼顺序转换，角色出现在大世界、队伍界面或详情页时识别部件。

## 鸣谢

- [XXMI-Launcher](https://github.com/SpectrumQT/XXMI-Launcher)：模式参考
- [GI-Model-Importer（GIMI）](https://github.com/SilentNightSound/GI-Model-Importer)：转换器读取的 MOD 格式和 PC 上的模型替换方式
- [3DMigoto](https://github.com/bo3b/3Dmigoto)：GIMI / XXMI 所基于的 D3D11 注入框架
- [Dobby](https://github.com/jmpews/Dobby)、[bcdec](https://github.com/iOrange/bcdec)、[Compose-Settings](https://github.com/alorma/Compose-Settings)、[LSPosed](https://github.com/LSPosed/LSPosed)：hook、BC 纹理解码、设置界面组件、模块运行环境
