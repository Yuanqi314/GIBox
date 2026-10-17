package com.gibox.tool

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Create
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Person
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.unit.dp
import com.alorma.compose.settings.ui.expressive.SettingsGroup
import com.alorma.compose.settings.ui.expressive.SettingsMenuLink
import com.alorma.compose.settings.ui.expressive.SettingsRadioButton

private val SERVERS = listOf("com.miHoYo.Yuanshen", "com.miHoYo.ys.bilibili")
private val LEVEL_NOTES = listOf("不记录", "只记录警告和错误", "再加导入、加载和替换过程（默认）", "再加逐次细节和转换明细，文件会变大")

/** Settings, built from Compose-Settings (expressive tiles): 通用 / 导入器 / 高级 / 关于, as XXMI's tabs. */
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
                SettingsGroup(title = { SectionTitle("导入器 · 日志文件输出详细程度") }, verticalArrangement = gap) {
                    val levels = Control.LOG_LEVELS
                    levels.forEachIndexed { index, level ->
                        SettingsRadioButton(
                            state = state.logLevel == level,
                            title = { Text(Control.LOG_LEVEL_LABELS[index]) },
                            subtitle = { Text(LEVEL_NOTES[index]) },
                            shapes = ListItemDefaults.segmentedShapes(index, levels.size + 1),
                            onClick = { state.changeLogLevel(level) },
                        )
                    }
                    SettingsMenuLink(
                        title = { Text("查看日志") },
                        subtitle = { Text("游戏和 GIBox 写同一个文件：" + AppLog.FILE) },
                        icon = { Icon(Icons.AutoMirrored.Filled.List, contentDescription = null) },
                        shapes = ListItemDefaults.segmentedShapes(levels.size, levels.size + 1),
                        onClick = { state.open(Screen.Log) },
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
                    "Mods 文件夹" to state.modsRoot.path,
                    "模型目录" to state.modelsRoot.path,
                    "骨骼库" to state.boneRoot.path,
                    "日志文件" to state.logFile.path,
                    "版本限定 Build ID" to BUILD_ID,
                )
                SettingsGroup(title = { SectionTitle("关于 GIBox $VERSION（点一项复制）") }, verticalArrangement = gap) {
                    paths.forEachIndexed { index, (label, value) ->
                        SettingsMenuLink(
                            title = { Text(label) },
                            subtitle = { Text(value) },
                            icon = { Icon(if (index == paths.size - 1) Icons.Filled.Info else Icons.Filled.Create, contentDescription = null) },
                            shapes = ListItemDefaults.segmentedShapes(index, paths.size),
                            onClick = { state.copy(label, value) },
                        )
                    }
                }
            }
        }
    }
}
