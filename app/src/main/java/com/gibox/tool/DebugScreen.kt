package com.gibox.tool

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Person
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.unit.dp
import com.alorma.compose.settings.ui.expressive.SettingsGroup
import com.alorma.compose.settings.ui.expressive.SettingsMenuLink
import com.alorma.compose.settings.ui.expressive.SettingsSwitch

/**
 * Debug and test tools, out of the way of the settings: every texture-only strategy, the test rounds and the index-buffer
 * hash diagnostic; the material and animation experiments; bone export and the old manual conversion.
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun DebugScreen(state: AppState) {
    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    val gap = Arrangement.spacedBy(ListItemDefaults.SegmentedGap)
    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        topBar = {
            LargeFlexibleTopAppBar(
                title = { Text("调试") },
                navigationIcon = { BackButton { state.back() } },
                scrollBehavior = scroll,
            )
        },
    ) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = padding.calculateTopPadding(), bottom = padding.calculateBottomPadding() + 24.dp),
        ) {
            item {
                // Texture-only mods (0.7.52): takes effect within a second; the hash diagnostic at the next game start.
                val rev = state.revision
                val strategy = remember(rev) { state.textureMods("strategy", "diagnose") }
                val ibHash = remember(rev) { state.textureModsFlag("ibHash") }
                SettingsGroup(title = { SectionTitle("纯贴图 MOD") }, verticalArrangement = gap) {
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(0, 3)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("替换方式（全部）", style = MaterialTheme.typography.titleSmall)
                            Text("对比用：直接改游戏材质（读进来的原始材质会等游戏的实例）、克隆当前材质（游戏在自己实例上的效果可能看不到）。切换会先还原。日志 texture-apply / texture-restore / texture-event。", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in TEXTURE_STRATEGIES) {
                                    FilterChip(selected = strategy == value, onClick = { state.setTextureMods("strategy", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                    SettingsMenuLink(
                        title = { Text("开始轮测") },
                        subtitle = { Text("角色在场时：自动、直接改材质、克隆材质各一轮，每轮保存基线 → 写入 → 读回 → 观察 4 秒 → 还原 → 核对还原；还原失败就停。首次出场、冷启动要重新进场景或重启才能测") },
                        icon = { Icon(Icons.Filled.Build, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(1, 3),
                        onClick = { state.startTextureTest() },
                    )
                    SettingsSwitch(
                        state = ibHash,
                        title = { Text("索引 hash 诊断（Vulkan）") },
                        subtitle = { Text("下次启动游戏生效。读取游戏上传的索引数据，按 3DMigoto 的算法算 hash：和 MOD 规则一致、且只对应一个 Mesh 的部件（刘海、眼睛这类分段起点分辨不出的）会被确认并存进映射，按替换方式写入；存下后关掉开关也照样用。日志 ib-hash / texture-hash-confirm，完整列表在 ib-hashes.txt") },
                        shapes = ListItemDefaults.segmentedShapes(2, 3),
                        onCheckedChange = { state.setTextureModsFlag("ibHash", it) },
                    )
                }
            }
            item {
                // The trace takes effect at once (with the log level 调试); game materials with the next switch-in.
                val rev = state.revision
                val games = remember(rev) { state.experiment("gameMaterials") }
                val trace = remember(rev) { state.experiment("glowTrace") }
                SettingsGroup(title = { SectionTitle("材质实验") }, verticalArrangement = gap) {
                    SettingsSwitch(
                        state = trace,
                        title = { Text("追踪游戏对材质的写入") },
                        subtitle = { Text("不改画面。切人和装上模型时，把谁改了哪个参数、改成什么写进日志（日志级别为“调试”时记录）") },
                        shapes = ListItemDefaults.segmentedShapes(0, 2),
                        onCheckedChange = { state.setExperiment("glowTrace", it) },
                    )
                    SettingsSwitch(
                        state = games,
                        title = { Text("只换网格，材质用游戏自己的") },
                        subtitle = { Text("贴图会不对；看还发不发光") },
                        shapes = ListItemDefaults.segmentedShapes(1, 2),
                        onCheckedChange = { state.setExperiment("gameMaterials", it) },
                    )
                }
            }
            item {
                val rev = state.revision
                val animation = remember(rev) { state.experimentInt("animationMode") }
                val logic = remember(rev) { state.experiment("logicCore") }
                SettingsGroup(title = { SectionTitle("模型动画实验") }, verticalArrangement = gap) {
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(0, 2)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("动画播放方式", style = MaterialTheme.typography.titleSmall)
                            Text("切换后自动重建动画模型。预热完成后开始播放；只影响带动画的 MOD。调试日志记录帧号、耗时和内存估算。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in listOf(0 to "缓存帧网格", 1 to "动态网格", 2 to "暂停（初始帧）")) {
                                    FilterChip(selected = animation == value, onClick = { state.setExperiment("animationMode", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                    SettingsSwitch(
                        state = logic,
                        title = { Text("由 MOD 自己的 INI 逻辑决定显示哪一帧、哪套款式") },
                        subtitle = { Text("3DMigoto 逻辑核心在游戏里执行 [Constants]、[Present]、TextureOverride，按选中的资源找已转换的帧或款式；款式的选择作为逻辑的输入；MOD 详情里的“按键”按 MOD 自己的 [Key]（切换 / 开关），persist 变量按模型保存。需要用 0.7.36 重新转换的 MOD（按键要 0.7.37），否则或出错时仍按原来的方式。日志看 logic-") },
                        shapes = ListItemDefaults.segmentedShapes(1, 2),
                        onCheckedChange = { state.setExperiment("logicCore", it) },
                    )
                }
            }
            item {
                SettingsGroup(title = { SectionTitle("骨骼和旧流程") }, verticalArrangement = gap) {
                    SettingsMenuLink(
                        title = { Text("导出当前角色骨骼") },
                        subtitle = { Text("游戏里有角色在场时，导出一份带时间的骨骼目录，供手动转换用") },
                        icon = { Icon(Icons.Filled.Person, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(0, 2),
                        onClick = { state.exportBones() },
                    )
                    SettingsMenuLink(
                        title = { Text("手动转换（旧流程）") },
                        subtitle = { Text("直接指定骨骼文件夹、角色 ID 和部件映射，不经过 Mods 列表") },
                        icon = { Icon(Icons.Filled.Build, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(1, 2),
                        onClick = { state.open(Screen.Manual) },
                    )
                }
            }
        }
    }
}
