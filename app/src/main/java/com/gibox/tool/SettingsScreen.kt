package com.gibox.tool

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Icon
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.unit.dp
import com.alorma.compose.settings.ui.expressive.SettingsGroup
import com.alorma.compose.settings.ui.expressive.SettingsMenuLink
import com.alorma.compose.settings.ui.expressive.SettingsRadioButton

private val SERVERS = listOf("com.miHoYo.Yuanshen", "com.miHoYo.ys.bilibili")

/** Texture-only strategies by control.json value, as the settings and the debug page name them. */
internal val TEXTURE_STRATEGIES = listOf("diagnose" to "只诊断", "auto" to "自动", "direct" to "直接改游戏材质的贴图", "clone" to "克隆当前材质后换贴图")

/** Settings: general access, texture-only mods, file directories; the debug page at the bottom. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun SettingsScreen(state: AppState) {
    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    val gap = Arrangement.spacedBy(ListItemDefaults.SegmentedGap)
    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        topBar = {
            LargeFlexibleTopAppBar(
                title = { Text("设置") },
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
                SettingsGroup(title = { SectionTitle("通用") }, verticalArrangement = gap) {
                    SERVERS.forEachIndexed { index, pkg ->
                        SettingsRadioButton(
                            state = state.game == pkg,
                            title = { Text(serverName(pkg)) },
                            subtitle = { Text(pkg) },
                            shapes = ListItemDefaults.segmentedShapes(index, SERVERS.size + 1),
                            onClick = { state.setServer(pkg) },
                        )
                    }
                    SettingsMenuLink(
                        title = { Text("文件访问权限") },
                        subtitle = { Text(if (state.storage) "已授予" else "未授予：点这里去授予") },
                        icon = { Icon(Icons.Filled.Lock, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(SERVERS.size, SERVERS.size + 1),
                        onClick = { state.grantStorage() },
                    )
                }
            }
            item {
                // Texture-only mods: write or not, and the LOD option. The other strategies are on the debug page.
                val rev = state.revision
                val strategy = remember(rev) { state.textureMods("strategy", "diagnose") }
                val lod = remember(rev) { state.textureMods("lod", "full") }
                SettingsGroup(title = { SectionTitle("纯贴图 MOD") }, verticalArrangement = gap) {
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(0, 2)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("替换方式", style = MaterialTheme.typography.titleSmall)
                            Text("自动：游戏自己的材质实例直接换贴图（出场光效等游戏效果照常），读进来的原始材质先复制再换。只写确认过的槽位（子网格起点或索引 hash），只写 diffuse 和 lightmap；切换会先还原。" + (if (strategy == "direct" || strategy == "clone") "\n当前用的是调试页里的“${TEXTURE_STRATEGIES.toMap()[strategy]}”。" else ""), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in listOf("diagnose" to "只诊断，不写入", "auto" to "自动（推荐）")) {
                                    FilterChip(selected = strategy == value, onClick = { state.setTextureMods("strategy", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(1, 2)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("低精度模型（LOD）", style = MaterialTheme.typography.titleSmall)
                            Text("大世界里常显示 Body_LOD1；完整模型多在队伍预览、角色详情里。迁移只用于渲染器名、网格、子网格数、材质数和槽位材质名都核对一致的 LOD。", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in listOf("full" to "只用完整模型确认的结果", "migrate" to "允许迁移到核对过的 LOD")) {
                                    FilterChip(selected = lod == value, onClick = { state.setTextureMods("lod", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                }
            }
            item {
                val paths = listOf(
                    "点击打开 Mods 文件夹" to state.modsRoot,
                    "点击打开模型文件夹" to state.modelsRoot,
                    "点击打开骨骼库文件夹" to state.boneRoot,
                    "分享日志文件" to state.logFile,
                )
                SettingsGroup(title = { SectionTitle("文件目录") }, verticalArrangement = gap) {
                    paths.forEachIndexed { index, (label, value) ->
                        SettingsMenuLink(
                            title = { Text(label) },
                            subtitle = { Text(value.path) },
                            icon = { Icon(if (index == paths.size - 1) Icons.Filled.Share else Icons.AutoMirrored.Filled.List, contentDescription = null) },
                            shapes = ListItemDefaults.segmentedShapes(index, paths.size),
                            onClick = { if (index == paths.size - 1) state.shareLog() else state.openFolder(value) },
                        )
                    }
                }
            }
            item {
                SettingsGroup(title = { SectionTitle("调试") }, verticalArrangement = gap) {
                    SettingsMenuLink(
                        title = { Text("调试") },
                        subtitle = { Text("替换方式对比、轮测、索引 hash 诊断、材质和动画实验、骨骼导出、手动转换（旧流程）") },
                        icon = { Icon(Icons.Filled.Build, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(0, 1),
                        onClick = { state.open(Screen.Debug) },
                    )
                }
            }
        }
    }
}
