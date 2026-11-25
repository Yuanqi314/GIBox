package com.gibox.tool

import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.updateTransition
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.tappableElement
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.res.vectorResource
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.itos.coui.effect.CouiFloatingRenderer
import com.itos.coui.effect.couiNavigationFeedback
import com.itos.coui.floatingactionbutton.CouiFabColor
import com.itos.coui.floatingactionbutton.CouiFabDefaults
import com.itos.coui.icons.CouiIcons
import com.itos.coui.layout.Scaffold
import com.itos.coui.navigation.CouiNavigationItem
import com.itos.coui.navigation.NavigationBar
import com.itos.coui.shape.CouiCapsuleShape
import com.itos.coui.text.CouiTextKind
import com.itos.coui.text.Text
import com.itos.coui.theme.CouiTheme

internal val LocalHomeTabInsets = compositionLocalOf<PaddingValues?> { null }

private val destinations = listOf(Screen.Mods, Screen.Accessibility, Screen.Settings, Screen.About)

@Composable
internal fun HomeTabs(state: AppState) {
    var selected by rememberSaveable { mutableIntStateOf(0) }
    var focusTarget by rememberSaveable { mutableStateOf<String?>(null) }
    val modSearch = rememberSaveable { mutableStateOf(false) }
    val keyboard = LocalSoftwareKeyboardController.current
    val rtl = LocalLayoutDirection.current == LayoutDirection.Rtl
    val select: (Int) -> Unit = { index ->
        if (index != selected) {
            keyboard?.hide()
            focusTarget = null
            selected = index
        }
    }
    val holder = rememberSaveableStateHolder()
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) state.importFrom(uri)
    }
    val items = listOf(
        CouiNavigationItem("模组管理", CouiIcons.Filled.Gamepad),
        CouiNavigationItem("辅助功能", CouiIcons.Filled.Accessibility),
        CouiNavigationItem("设置", CouiIcons.Filled.SettingDetail),
        CouiNavigationItem("关于", ImageVector.vectorResource(R.drawable.gibox_info)),
    )
    val transition = updateTransition(selected, label = "底栏切换")
    val switching = transition.currentState != selected || transition.isRunning
    val stickerPageActive = LocalStickerPageActive.current && !switching
    val showImport = selected == 0 && state.libraryReady && !modSearch.value
    BackHandler(selected != 0) { select(0) }
    val ime = if (LocalWindowInfo.current.isWindowFocused) Modifier.imePadding() else Modifier
    Scaffold(modifier = ime, bottomBar = {
        HomeNavigationBar(items, selected, select, showImport) {
            if (state.storage) importer.launch(zipTypes) else state.needStorage()
        }
    }) { insets ->
        CompositionLocalProvider(
            LocalHomeTabInsets provides insets,
            LocalStickerPageActive provides stickerPageActive,
        ) {
            transition.AnimatedContent(
                modifier = Modifier.fillMaxSize().clipToBounds(),
                transitionSpec = {
                    val direction = if ((targetState > initialState) != rtl) 1 else -1
                    val enter = slideInHorizontally(tween(210)) { direction * it / 10 } + fadeIn(tween(190))
                    val exit = slideOutHorizontally(tween(160)) { -direction * it / 12 } + fadeOut(tween(130))
                    (enter togetherWith exit).using(null)
                },
            ) { index ->
                holder.SaveableStateProvider(index) {
                    when (destinations[index]) {
                        Screen.Mods -> ModManagementScreen(state, searchState = modSearch, openFeature = { feature ->
                            val tab = destinations.indexOf(feature.page)
                            if (tab >= 0) {
                                select(tab)
                                focusTarget = feature.title
                            } else state.openFeature(feature)
                        })
                        Screen.Accessibility -> key(state.game) { AccessibilityScreen(state, focusTarget) }
                        Screen.Settings -> key(state.game) { SettingsScreen(state, focusTarget) }
                        Screen.About -> AboutScreen(state)
                        else -> error("Unknown home tab")
                    }
                }
            }
        }
    }
}

@Composable
private fun HomeNavigationBar(
    items: List<CouiNavigationItem>,
    selected: Int,
    select: (Int) -> Unit,
    showImport: Boolean,
    importMod: () -> Unit,
) {
    val dark = CouiTheme.isDark
    val ink = if (dark) Color.White else Color.Black
    BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.BottomCenter) {
        val margin = 16.dp
        val tabPadding = 12.dp
        val tabGap = 8.dp
        val accessorySize = 56.dp
        val accessoryGap = 12.dp
        val accessorySlot = accessorySize + accessoryGap
        val capsuleChrome = tabPadding * 2 + tabGap * (items.size - 1)
        val maxCapsuleWidth = capsuleChrome + 70.dp * items.size
        val barWidth = minOf(maxWidth, margin * 2 + maxCapsuleWidth + accessorySlot)
        val tabWidth = (barWidth - margin * 2 - accessorySlot - capsuleChrome) / items.size
        val density = LocalDensity.current
        val labelWidth = with(density) { 10.sp.toDp() } * items.maxOf { it.title.length } + 2.dp
        if (Build.VERSION.SDK_INT >= 34 && tabWidth >= maxOf(48.dp, labelWidth)) {
            val renderer = remember { runCatching { CouiFloatingRenderer() }.getOrNull() }
            if (renderer != null) {
                Layout(
                    modifier = Modifier.width(barWidth),
                    content = {
                        NavigationBar(
                            renderer = renderer,
                            items = items,
                            selectedIndex = selected,
                            rightIcon = CouiIcons.Filled.Add,
                            rightDescription = "导入 MOD",
                            rightVisible = showImport,
                            onRightClick = if (showImport) importMod else null,
                            rightContainerColor = CouiFabDefaults.containerColor(CouiFabColor.InfraredRemote, dark),
                            selectedColor = ink,
                            unselectedColor = ink.copy(alpha = .68f),
                            onSelection = select,
                        )
                    },
                ) { measurables, constraints ->
                    // alpha11 still measures a hidden left accessory. Move just that slot outside the row.
                    val unusedSlot = accessorySlot.roundToPx()
                    val width = constraints.maxWidth
                    val measuredWidth = width + unusedSlot
                    val bar = measurables.single().measure(constraints.copy(minWidth = measuredWidth, maxWidth = measuredWidth))
                    layout(width, bar.height) { bar.placeRelative(-unusedSlot, 0) }
                }
                return@BoxWithConstraints
            }
        }
        val bottomMargin = if (WindowInsets.tappableElement.getBottom(density) > 0) 16.dp else 24.dp
        val sideMargin = if (tabWidth >= 48.dp) margin else 8.dp
        val innerPadding = if (tabWidth >= 48.dp) tabPadding else 8.dp
        Row(
            modifier = Modifier.width(barWidth)
                .windowInsetsPadding(WindowInsets.tappableElement.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom))
                .padding(start = sideMargin, end = sideMargin, top = 16.dp, bottom = bottomMargin),
            horizontalArrangement = Arrangement.spacedBy(accessoryGap),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PlainHomeNavigationBar(items, selected, select, ink, Modifier.weight(1f), tabGap, innerPadding)
            Box(Modifier.size(accessorySize), contentAlignment = Alignment.Center) {
                androidx.compose.animation.AnimatedVisibility(showImport, enter = fadeIn(), exit = fadeOut()) {
                    ImportModButton { if (showImport) importMod() }
                }
            }
        }
    }
}

@Composable
private fun PlainHomeNavigationBar(items: List<CouiNavigationItem>, selected: Int, select: (Int) -> Unit,
    ink: Color, modifier: Modifier, gap: Dp, innerPadding: Dp) {
    Row(modifier.clip(CouiCapsuleShape).background(CouiTheme.colorScheme.card)
        .padding(horizontal = innerPadding).heightIn(min = 56.dp).selectableGroup(),
        horizontalArrangement = Arrangement.spacedBy(gap), verticalAlignment = Alignment.CenterVertically) {
        items.forEachIndexed { index, item ->
            val source = remember { MutableInteractionSource() }
            val color = if (index == selected) ink else ink.copy(alpha = .68f)
            Column(Modifier.weight(1f).clip(CouiCapsuleShape).couiNavigationFeedback(CouiTheme.isDark, source)
                .selectable(index == selected, source, null, role = Role.Tab) { select(index) }
                .padding(vertical = 8.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Image(item.icon, null, Modifier.size(24.dp), colorFilter = ColorFilter.tint(color))
                Text(text = item.title, kind = CouiTextKind.GroupTitle, color = color, textAlign = TextAlign.Center)
            }
        }
    }
}
