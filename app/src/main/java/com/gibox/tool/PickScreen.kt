package com.gibox.tool

import android.graphics.BlendMode
import android.os.Build
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.itos.coui.backdrop.CouiBackdrop
import com.itos.coui.backdrop.couiBackdrop
import com.itos.coui.backdrop.drawCouiBackdrop
import com.itos.coui.backdrop.rememberCouiBackdrop
import com.itos.coui.dialog.CouiDialogDefaults
import com.itos.coui.effect.CouiFloatingRenderer
import com.itos.coui.effect.couiButtonFeedback
import com.itos.coui.layout.Divider
import com.itos.coui.layout.Footer
import com.itos.coui.layout.Group
import com.itos.coui.radiobutton.CouiRadioButtonDefaults
import com.itos.coui.radiobutton.RadioButton
import com.itos.coui.scroll.rememberCouiOverscrollEffect
import com.itos.coui.shape.CouiCapsuleShape
import com.itos.coui.text.CouiTextKind
import com.itos.coui.text.Text
import com.itos.coui.theme.CouiColor
import com.itos.coui.theme.CouiTheme
import com.itos.coui.topappbar.CouiTopAppBarButtonDefaults
import com.itos.coui.topappbar.TopAppBarButton
import com.itos.coui.topappbar.TopAppBarSurface
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** A scheme's file name without .ini; the folder too when two schemes share a name. */
fun schemeLabel(rel: String, all: List<String>): String {
    val name = rel.substringAfterLast('/').replace(Regex("(?i)\\.ini$"), "")
    return if (all.count { it.substringAfterLast('/').equals(rel.substringAfterLast('/'), true) } > 1) rel.replace(Regex("(?i)\\.ini$"), "") else name
}

private data class PickOutfitDetails(val prefab: String, val recorded: Set<String>, val hint: String)

/**
 * Full page for choosing the mod's character: search by ID or name, the guessed character first marked （推荐）, one tap
 * selects, and only 确定 starts the import (or the re-conversion). Leaving the page imports nothing. A character with
 * skins lists its outfits under it (原皮 and each skin): the mod is converted for the one chosen, the one its INI hashes
 * name chosen first. Its mods stay in that character's list either way.
 */
@Composable
fun PickScreen(state: AppState, pick: Screen.Pick) {
    val guess = remember(pick) {
        Characters.Guess().apply { recommended.addAll(pick.recommendedIds.mapNotNull(Characters::byId)) }
    }
    // Off the main thread: the first call reads the skeleton table made from the game's files (17 MB).
    val recorded by produceState(initialValue = emptySet<Int>(), pick, state.game) { value = withContext(Dispatchers.IO) { state.recorded() } }
    var query by rememberSaveable(pick) { mutableStateOf("") }
    var chosen by rememberSaveable(pick) { mutableIntStateOf(0) }
    var selectionRevision by remember(pick) { mutableIntStateOf(0) }
    val list = remember(guess, query) { Characters.ordered(guess, query) }
    val selected = Characters.byId(chosen)
    val dir = pick.pending ?: pick.source
    val ini by produceState<Pair<List<Pair<String, String>>, String?>?>(null, dir) {
        value = withContext(Dispatchers.IO) {
            val choices = state.schemes(dir)
            choices.map { it to schemeLabel(it, choices) } to state.scheme(dir).takeIf { it in choices }
        }
    }
    val schemes = ini?.first.orEmpty()
    var scheme by remember(pick, ini) { mutableStateOf(ini?.second) }
    val needScheme = schemes.size > 1 && scheme == null
    val outfits = remember(chosen) { state.outfits(chosen) }
    // Keep each row's own details available until its exit animation finishes.
    val outfitDetails = remember(pick, state.game) { mutableStateMapOf<Int, PickOutfitDetails>() }
    LaunchedEffect(pick, chosen, selectionRevision, state.game) {
        val id = chosen
        val revision = selectionRevision
        if (outfits.isEmpty()) return@LaunchedEffect
        val details = withContext(Dispatchers.IO) {
            PickOutfitDetails(
                prefab = state.suggestedOutfit(pick, id),
                recorded = state.recordedOutfits(id),
                hint = "",
            )
        }
        if (revision != selectionRevision) return@LaunchedEffect
        outfitDetails[id] = details
        val hint = withContext(Dispatchers.IO) { state.outfitGuessText(pick, id) }
        if (revision == selectionRevision) {
            outfitDetails[id]?.let { outfitDetails[id] = it.copy(hint = hint) }
        }
    }
    val outfit = outfitDetails[chosen]?.prefab.orEmpty()
    val chosenOutfit = outfits.firstOrNull { it.prefab == outfit }
    val boneHint by produceState(initialValue = "", chosen, outfit, state.game) {
        value = withContext(Dispatchers.IO) { state.boneImportHint(chosen, outfit) }
    }
    val dark = CouiTheme.isDark
    val radioColors = remember(dark) {
        CouiRadioButtonDefaults.colors(if (dark) CouiColor.Night.BLUE else CouiColor.Day.BLUE, dark)
    }
    val renderer = if (Build.VERSION.SDK_INT >= 34) {
        remember { runCatching { CouiFloatingRenderer.standalone() }.getOrNull() }
    } else null
    val fallbackBackdrop = if (renderer == null) rememberCouiBackdrop() else null
    val listSurface = if (fallbackBackdrop != null) {
        Modifier.couiBackdrop(fallbackBackdrop).background(CouiTheme.colorScheme.background)
    } else Modifier

    Page("选择分类或角色", state::back, topContent = {
        SearchControl(query, { query = it }, "搜索 ID 或名称")
    }, bottom = {
        PickConfirmButton(
            enabled = state.libraryReady && selected != null && ini != null && !needScheme && (outfits.isEmpty() || chosenOutfit != null),
            renderer = renderer,
            fallbackBackdrop = fallbackBackdrop,
        ) {
            selected?.let { state.confirm(pick, it, if (schemes.size > 1) scheme else null, chosenOutfit?.prefab) }
        }
    }) { padding ->
        LazyColumn(
            Modifier.fillMaxSize().then(listSurface).padding(top = padding.calculateTopPadding())
                .consumeWindowInsets(padding).selectableGroup(),
            contentPadding = PaddingValues(bottom = padding.calculateBottomPadding()),
            overscrollEffect = rememberCouiOverscrollEffect(),
        ) {
            item {
                Group {
                    ActionRow(pick.modName, when {
                        ini == null -> "正在读取 INI 方案…"
                        needScheme -> "先选择一套 INI 方案"
                        selected == null -> "点一个角色，再点确定"
                        else -> "${selected.id} · $boneHint"
                    })
                    if (schemes.size > 1) {
                        Divider()
                        ChoiceRow("INI 方案", scheme, schemes,
                            "有 ${schemes.size} 套方案，之后也可更换") { scheme = it }
                    }
                }
            }
            item {
                val names = guess.recommended.joinToString("、") { it.name }
                Footer(text = if (names.isEmpty()) "没认出角色，请按 ID 或名称查找" else "根据 MOD 名称和 hash 推荐：$names")
            }
            if (list.isEmpty()) item { Footer(text = "没有匹配的角色") }
            items(list, key = { it.id }) { avatar ->
                val recommended = guess.recommends(avatar.id)
                val checked = chosen == avatar.id
                val avatarOutfits = remember(avatar.id) { state.outfits(avatar.id) }
                val interaction = remember { MutableInteractionSource() }
                Group {
                    ActionRow(avatar.name + if (recommended) "（推荐）" else "", when {
                        Characters.isLoading(avatar.id) -> "加载图片、头像和名片、天空门、浪船"
                        Characters.isScene(avatar.id) -> "NPC、怪物、武器、风之翼" + if (avatar.id == pick.current) " · 当前" else ""
                        else -> "${avatar.id}" + (if (recorded.contains(avatar.id)) " · 已记录骨骼" else "") +
                            (if (avatar.id == pick.current) " · 当前" else "")
                    }, modifier = Modifier.couiButtonFeedback(dark, interaction, true, list = true)
                        .selectable(checked, interaction, null, role = Role.RadioButton) {
                            if (chosen != avatar.id) {
                                outfitDetails.remove(avatar.id)
                                chosen = avatar.id
                                selectionRevision++
                            }
                        },
                        trailing = { RadioButton(selected = checked, colors = radioColors) })
                    if (avatarOutfits.isNotEmpty()) {
                        AnimatedVisibility(
                            visible = checked,
                            enter = expandVertically(tween(260), expandFrom = Alignment.Top) + fadeIn(tween(180)),
                            exit = shrinkVertically(tween(200), shrinkTowards = Alignment.Top) + fadeOut(tween(150)),
                        ) {
                            val details = outfitDetails[avatar.id]
                            Column(Modifier.animateContentSize(tween(200))) {
                                Divider()
                                ChoiceRow(
                                    "衣装", details?.prefab, avatarOutfits.map { row ->
                                        row.prefab to (row.name + if (details?.recorded?.contains(row.prefab) == true)
                                            " · 已记录骨骼" else " · 未记录骨骼")
                                    },
                                    details?.hint.orEmpty().ifBlank { "未记录的衣装可在游戏衣装页预览后自动采集骨骼" },
                                    enabled = checked && details != null,
                                ) { prefab ->
                                    if (chosen == avatar.id) {
                                        outfitDetails[avatar.id]?.let { outfitDetails[avatar.id] = it.copy(prefab = prefab) }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PickConfirmButton(
    enabled: Boolean,
    renderer: CouiFloatingRenderer?,
    fallbackBackdrop: CouiBackdrop?,
    click: () -> Unit,
) {
    val frame = Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 12.dp)
    val theme = CouiTheme.colorScheme
    val primary = CouiDialogDefaults.primaryActionColors(theme.accent)
    val surface = if (enabled) primary.surface else primary.disabledSurface
    val ink = if (enabled) primary.content else theme.onSurfaceVariant
    if (Build.VERSION.SDK_INT >= 34 && renderer != null) {
        TopAppBarSurface(
            modifier = frame,
            bottomExtension = 0.dp,
            backgroundAlpha = 0f,
            gradientColor = Color.Transparent,
        ) {
            val style = CouiTopAppBarButtonDefaults.style()
            TopAppBarButton(
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                onClick = click,
                enabled = enabled,
                text = true,
                renderer = renderer,
                style = style.copy(material = { capsule ->
                    style.material(capsule).copy(
                        firstColor = surface,
                        firstBlendMode = BlendMode.SRC_OVER,
                        secondColor = Color.Transparent,
                    )
                }),
            ) {
                Text(modifier = Modifier.fillMaxWidth(), text = "确定", kind = CouiTextKind.ItemTitle,
                    color = ink, textAlign = TextAlign.Center)
            }
        }
        return
    }

    val source = remember { MutableInteractionSource() }
    Box(
        frame.heightIn(min = 48.dp)
            .drawCouiBackdrop(
                checkNotNull(fallbackBackdrop), shape = { CouiCapsuleShape }, effects = { blur(16.dp.toPx()) },
                onDrawSurface = { drawRect(surface) },
            )
            .clip(CouiCapsuleShape)
            .couiButtonFeedback(interactionSource = source, enabled = enabled)
            .clickable(source, indication = null, enabled = enabled, role = Role.Button, onClick = click)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text = "确定", kind = CouiTextKind.ItemTitle, color = ink)
    }
}
