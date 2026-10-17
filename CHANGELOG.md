# 0.7.2-source

- **`drawindexed = 0, …`（画 0 个索引）不再报“drawindexed 数量”**，当作什么都不画（有的 MOD 用它占位或隐藏部件）。
- **款式分组更细**：
  - 同一个 IB hash 下的各部件段（Head / Body / Dress 这类，各有自己的 ib）不再因为 hash 相同就绑成一组；只有 hash 级、自己没有 ib 的段（如 `drawindexed = auto`）带条件时，才和各部件段一组。
  - 之前绫华的 `$horns、$hair、$shirt、$haircolor、$dress、$dresscolor` 很可能就是这样被绑成了一组（6336 种，超过上限）。
- **按结构分不出来的相互影响，靠实测合并**：每两组都同时改一改（两组合计 16 种以内全试，否则抽 8 种），画出来的和分别改动拼起来的对不上，就把这两组合并再查；最后再随机抽查多组同时改。之前一发现对不上就整体退回默认款式。
- 测试：CommandList 里绑了随变量换贴图的 Resource（结构上看不出关联）时，两组被实测合并、贴图跟着变量走；`drawindexed = 0` 被忽略。

