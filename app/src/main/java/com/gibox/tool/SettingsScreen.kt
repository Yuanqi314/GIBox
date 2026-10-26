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
                // Only while the log level is 调试. The trace and the slot textures take effect at once; the other experiments one at
                // a time, then restart the game and switch characters once.
                val rev = state.revision
                val games = remember(rev) { state.experiment("gameMaterials") }
                val colors = remember(rev) { state.experiment("neutralColors") }
                val uv2 = remember(rev) { state.experiment("noUv2") }
                val separate = remember(rev) { state.experiment("separateDraws") }
                val trace = remember(rev) { state.experiment("glowTrace") }
                val ms = remember(rev) { state.experimentMs() }
                val slots = remember(rev) { state.experimentInt("slotTextures") }
                SettingsGroup(title = { SectionTitle("材质实验（调试）") }, verticalArrangement = gap) {
                    SettingsSwitch(
                        state = trace,
                        title = { Text("追踪游戏对材质的写入") },
                        subtitle = { Text("不改画面。切人、装上模型和「标记画面异常」时，把谁改了哪个参数、改成什么写进日志") },
                        shapes = ListItemDefaults.segmentedShapes(0, 7),
                        onCheckedChange = { state.setExperiment("glowTrace", it) },
                    )
                    SettingsSwitch(
                        state = games,
                        title = { Text("只换网格，材质用游戏自己的") },
                        subtitle = { Text("贴图会不对；看还发不发光") },
                        shapes = ListItemDefaults.segmentedShapes(1, 7),
                        onCheckedChange = { state.setExperiment("gameMaterials", it) },
                    )
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(2, 7)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("贴图用按槽覆盖（保留游戏材质）", style = MaterialTheme.typography.titleSmall)
                            Text("不复制材质，MOD 的贴图按绘制槽盖到游戏材质上；选后马上生效，日志里看 slot-textures", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in listOf(0 to "关（复制材质）", 1 to "按槽属性块", 2 to "按槽自定义贴图")) {
                                    FilterChip(selected = slots == value, onClick = { state.setExperiment("slotTextures", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(3, 7)) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("挂上后第一次重新复制材质", style = MaterialTheme.typography.titleSmall)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                for ((value, label) in listOf(0 to "默认 1.5 秒", 300 to "0.3 秒", 600 to "0.6 秒")) {
                                    FilterChip(selected = ms == value, onClick = { state.setExperiment("firstRefreshMs", value) }, label = { Text(label) })
                                }
                            }
                        }
                    }
                    SettingsSwitch(
                        state = colors,
                        title = { Text("顶点色改为原模型的常用值") },
                        subtitle = { Text("模型会重新构建") },
                        shapes = ListItemDefaults.segmentedShapes(4, 7),
                        onCheckedChange = { state.setExperiment("neutralColors", it) },
                    )
                    SettingsSwitch(
                        state = uv2,
                        title = { Text("第二套 UV 清零") },
                        subtitle = { Text("模型会重新构建") },
                        shapes = ListItemDefaults.segmentedShapes(5, 7),
                        onCheckedChange = { state.setExperiment("noUv2", it) },
                    )
                    SettingsSwitch(
                        state = separate,
                        title = { Text("不合并相邻绘制段") },
                        subtitle = { Text("对照用：每个绘制段单独一个子网格（0.7.22 及以前的做法）；模型会重新构建，日志里看 submeshes") },
                        shapes = ListItemDefaults.segmentedShapes(6, 7),
                        onCheckedChange = { state.setExperiment("separateDraws", it) },
                    )
                }
            }
            if (state.logLevel == "debug") item {
                val rev = state.revision
                val animation = remember(rev) { state.experimentInt("animationMode") }
                SettingsGroup(title = { SectionTitle("模型动画实验（调试）") }, verticalArrangement = gap) {
                    SegmentedListItem(shapes = ListItemDefaults.segmentedShapes(0, 1)) {
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
