package com.gibox.tool

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** The mods of one character: switch each on or off here, tap one for its details. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun CharacterScreen(state: AppState, id: Int) {
    val group = state.group(id)
    val mods = group?.mods.orEmpty()
    val title = group?.title ?: if (id == 0) "未选择角色" else Characters.nameOf(id)
    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        topBar = {
            LargeFlexibleTopAppBar(
                title = { Text(title) },
                subtitle = { Text("${mods.size} 个 MOD" + if (id != 0) " · $id" else "") },
                navigationIcon = { BackButton { state.back() } },
                scrollBehavior = scroll,
            )
        },
    ) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = padding.calculateTopPadding() + 8.dp, bottom = padding.calculateBottomPadding() + 24.dp),
            verticalArrangement = Arrangement.spacedBy(ListItemDefaults.SegmentedGap),
        ) {
            if (mods.isEmpty()) item { EmptyNote("这个角色下没有 MOD 了") }
            itemsIndexed(mods, key = { _, m -> m.name }) { index, m ->
                val on = m.converted && state.selected(m.name)
                val label = state.label(m.name)
                SegmentedListItem(
                    onClick = { state.open(Screen.Detail(m.name)) },
                    shapes = ListItemDefaults.segmentedShapes(index, mods.size),
                    supportingContent = {
                        val parts = mutableListOf(if (!m.converted) "未转换" else if (on) "已开启" else "未开启")
                        if (m.outdated) parts.add("需重新转换")
                        if (label != m.name) parts.add(m.name)
                        Text(
                            parts.joinToString(" · "),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            color = if (m.outdated) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    trailingContent = {
                        Switch(checked = on, onCheckedChange = { state.setSelected(m, it) }, enabled = m.converted)
                    },
                ) {
                    Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}
