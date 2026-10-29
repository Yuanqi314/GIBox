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
import androidx.compose.material.icons.filled.Person
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
import com.alorma.compose.settings.ui.expressive.SettingsSwitch

private val SERVERS = listOf("com.miHoYo.Yuanshen", "com.miHoYo.ys.bilibili")

/** Settings: general access, a log shortcut, advanced tools and file directories. */
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
                SettingsGroup(title = { SectionTitle("日志") }, verticalArrangement = gap) {
                    SettingsMenuLink(
                        title = { Text("查看运行日志") },
                        subtitle = { Text("游戏和 GIBox 写同一个文件：" + AppLog.FILE) },
                        icon = { Icon(Icons.AutoMirrored.Filled.List, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(0, 1),
                        onClick = { state.open(Screen.Log) },
                    )
                }
            }
            if (state.logLevel == "debug") item {
                // Only while the log level is 调试. The trace takes effect at once; game materials with the next switch-in.
                val rev = state.revision
                val games = remember(rev) { state.experiment("gameMaterials") }
                val trace = remember(rev) { state.experiment("glowTrace") }
                SettingsGroup(title = { SectionTitle("材质实验（调试）") }, verticalArrangement = gap) {
                    SettingsSwitch(
                        state = trace,
                        title = { Text("追踪游戏对材质的写入") },
                        subtitle = { Text("不改画面。切人和装上模型时，把谁改了哪个参数、改成什么写进日志") },
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
            if (state.logLevel == "debug") item {
                val rev = state.revision
                val animation = remember(rev) { state.experimentInt("animationMode") }
                val logic = remember(rev) { state.experiment("logicCore") }
                SettingsGroup(title = { SectionTitle("模型动画实验（调试）") }, verticalArrangement = gap) {
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
                // Texture-only mods (0.7.52): how the game side puts their textures on; takes effect within a second.
                val rev = state.revision
                val strategy = remember(rev) { state.textureMods("strategy", "diagnose") }
                val lod = remember(rev) { state.textureMods("lod", "full") }
                SettingsGroup(title = { SectionTitle("纯贴图 MOD（实验）") }, verticalArrangement = gap) {
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(0, 3)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("替换策略", style = MaterialTheme.typography.titleSmall)
                            Text("只写确定匹配的槽位，只写 diffuse（_MainTex）和 lightmap（_LightMapTex）；切换策略会先还原。日志 texture-apply / texture-restore 记录写到了谁、有没有被覆盖、能否还原。", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in listOf("diagnose" to "只诊断", "direct" to "直接改游戏材质的贴图", "clone" to "克隆当前材质后换贴图")) {
                                    FilterChip(selected = strategy == value, onClick = { state.setTextureMods("strategy", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(1, 3)) {
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
                    SettingsMenuLink(
                        title = { Text("开始轮测") },
                        subtitle = { Text("角色在场时：直接改材质、克隆材质各一轮，每轮保存基线 → 写入 → 读回 → 观察 4 秒 → 还原 → 核对还原；还原失败就停。首次出场、冷启动要重新进场景或重启才能测") },
                        icon = { Icon(Icons.Filled.Build, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(2, 3),
                        onClick = { state.startTextureTest() },
                    )
                }
            }
            item {
                SettingsGroup(title = { SectionTitle("高级") }, verticalArrangement = gap) {
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
        }
    }
}
