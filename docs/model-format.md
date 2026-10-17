# GIBox 模型格式（版本 2）

GIBox 自己设计的格式，读写两端都是本工程的代码：转换器 `ModelConverter.java` 写，`model.hpp` 读；App 读清单用 `ModelManifest.java`。

0.7.0 起转换器写版本 2；版本 1（0.6.x 转换的模型）照样能读，不用重新转换。两者只在款式（`variants`、`draws`）和骨骼父节点上不同，见下面“版本 1”。

**`model.json`**（清单，UTF-8 JSON，最大 4 MB）：

| 字段 | 内容 |
|---|---|
| `format` / `version` | `"gibox-model"` / `2`（读 1 和 2）。版本不认识就拒绝（不会按旧规则硬读） |
| `name`、`generation` | 模型名；每次转换唯一的代号 |
| `character` | `id`（0 或 10000000–10001999）、`names`（`[a-z0-9]{3,40}`，最多 16 个）；两者至少有一个 |
| `flags` | `originalMaterials`、`diffuseOnly`（可选，手工改） |
| `variants` | 可选：`vars`（`$名字` + 可选值，最多 16 个，每个最多 64 个值）、`default`（每个变量的初始值，必须是它的可选值） |
| `blob` | `file`（必须是 `model.bin`）、`size`、`crc32` |
| `parts[]` | 1–64 个：`name`（`[A-Za-z0-9_-]`，不重复）、`renderer`、`bones`（1–512）、`vertices`（≤ 500000）、`indices`（3 的倍数，≤ 1500 万）、`data`、可选 `skeleton`（`bones` 骨骼名 + `bindposes`，可选 `parents`：每根骨骼的父骨骼序号，-1 为没有，不能成环）、可选 `target`（原 Mesh 特征：渲染器、Mesh 名、顶点数、子网格数、骨骼数）、`draws`（1–1024 段：`start`、`count`、`material`、`slot` < 64，可选 `when`） |
| `materials[]` | `name`；`original: true` 用游戏原材质，否则 `main` / `lightmap` / `normalmap`：`width`、`height`（≤ 4096）、`data`（RGBA8，自下而上） |
| `source` | 转换来源（Mods 里的路径、内容签名），只给 App 看 |

**`when`**（绘制段什么时候画）：`{"vars":["$halo","$legs"],"allow":[[3,1],[0,0]]}`，列出这段依赖的变量，和要画它的取值组合（最多 4096 组）；当前取值在 `allow` 里就画，没有 `when` 就一直画。同一时刻最多 64 段；一段都不画时部件显示为空。转换器把 INI 里 `if` 读到的变量的所有取值都算一遍（最多 4096 种），每段只记它真正依赖的变量，所以不受组合数限制；只在按键或 `[Present]` 里出现的变量（如 `$active`）、改了也不影响画面的变量，不会出现在款式里。

**`model.bin`**：所有数组依次排放，每段从 16 字节边界开始。`data` 里每段写成 `[偏移, 长度]`：`positions` / `normals` 12×顶点数，`uv` 8×，`indices` 4×索引数，`weights` 32×（4 个权重 + 4 个骨骼序号），可选 `uv2`–`uv4` 8×、`colors` 4×、`tangents` 16×。

**读取时检查**（全部通过才交给游戏，任一项不对就整份拒绝并在状态里写明哪一项）：文件大小与清单一致、CRC-32 一致；每段对齐、长度正好、不越界；浮点数都是有限值；索引都小于顶点数；每个顶点权重非负、和为 1（±0.001）、骨骼序号小于部件骨骼数；绘制段范围有效、材质存在、`when` 只引用声明过的变量和它的可选值；`skeleton` 的骨骼数与部件一致；`target`、款式、角色字段按上表校验。

**版本 1**：`variants` 是 `vars` + `combos`（1–32 种，每种给每个变量赋值、互不重复）+ `default`（组合序号）；每个部件的 `draws` 是每种组合一组绘制段列表；`skeleton` 没有 `parents`。读进来后按“每段属于哪些组合”换成上面的 `when`，不在列表里的取值组合按默认组合显示，和 0.6.x 的表现一样。

**`learned.json`**（游戏写）：`{"format":"gibox-learned","version":1,"id":…,"targets":{"Part0":{…}}}`，记下在游戏里认出的原 Mesh 特征，以及按名字导入的模型认出的角色 ID。读的时候只采用渲染器对得上的条目。

**`skeleton.json`**（游戏写，转换器读）：`{"format":"gibox-skeleton","version":1,"id":…,"root":"Scene/…/Avatar_…","renderers":[{"name","target","bones","parents","bindposes"}]}`，bindpose 以 9 位有效数字写出，读回与原 float 完全一致。

