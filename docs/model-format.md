# GIBox 模型格式（版本 1）

GIBox 自己设计的格式，读写两端都是本工程的代码：转换器 `ModelConverter.java` 写，`model.hpp` 读；App 读清单用 `ModelManifest.java`。

**`model.json`**（清单，UTF-8 JSON，最大 4 MB）：

| 字段 | 内容 |
|---|---|
| `format` / `version` | `"gibox-model"` / `1`。版本不认识就拒绝（不会按旧规则硬读） |
| `name`、`generation` | 模型名；每次转换唯一的代号 |
| `character` | `id`（0 或 10000000–10001999）、`names`（`[a-z0-9]{3,40}`，最多 16 个）；两者至少有一个 |
| `flags` | `originalMaterials`、`diffuseOnly`（可选，手工改） |
| `variants` | 可选：`vars`（`$名字` + 可选值，最多 16 个）、`combos`（1–32 种，每种给每个变量赋值、互不重复）、`default` |
| `blob` | `file`（必须是 `model.bin`）、`size`、`crc32` |
| `parts[]` | 1–64 个：`name`（`[A-Za-z0-9_-]`，不重复）、`renderer`、`bones`（1–512）、`vertices`（≤ 500000）、`indices`（3 的倍数，≤ 1500 万）、`data`、可选 `skeleton`（`bones` 骨骼名 + `bindposes`）、可选 `target`（原 Mesh 特征：渲染器、Mesh 名、顶点数、子网格数、骨骼数）、`draws`（每种组合一组：`start`、`count`、`material`、`slot` < 64） |
| `materials[]` | `name`；`original: true` 用游戏原材质，否则 `main` / `lightmap` / `normalmap`：`width`、`height`（≤ 4096）、`data`（RGBA8，自下而上） |
| `source` | 转换来源（Mods 里的路径、内容签名），只给 App 看 |

**`model.bin`**：所有数组依次排放，每段从 16 字节边界开始。`data` 里每段写成 `[偏移, 长度]`：`positions` / `normals` 12×顶点数，`uv` 8×，`indices` 4×索引数，`weights` 32×（4 个权重 + 4 个骨骼序号），可选 `uv2`–`uv4` 8×、`colors` 4×、`tangents` 16×。

**读取时检查**（全部通过才交给游戏，任一项不对就整份拒绝并在状态里写明哪一项）：文件大小与清单一致、CRC-32 一致；每段对齐、长度正好、不越界；浮点数都是有限值；索引都小于顶点数；每个顶点权重非负、和为 1（±0.001）、骨骼序号小于部件骨骼数；绘制段范围有效、材质存在；`skeleton` 的骨骼数与部件一致；`target`、款式、角色字段按上表校验。

**`learned.json`**（游戏写）：`{"format":"gibox-learned","version":1,"id":…,"targets":{"Part0":{…}}}`，记下在游戏里认出的原 Mesh 特征，以及按名字导入的模型认出的角色 ID。读的时候只采用渲染器对得上的条目。

**`skeleton.json`**（游戏写，转换器读）：`{"format":"gibox-skeleton","version":1,"id":…,"root":"Scene/…/Avatar_…","renderers":[{"name","target","bones","parents","bindposes"}]}`，bindpose 以 9 位有效数字写出，读回与原 float 完全一致。

