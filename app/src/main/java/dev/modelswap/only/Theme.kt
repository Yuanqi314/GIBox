package dev.modelswap.only

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ContainedLoadingIndicator
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialExpressiveTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MotionScheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties

/** Material 3 Expressive: dynamic colour on Android 12+, expressive motion, light / dark from the system. */
@Composable
fun GIBoxTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val dark = isSystemInDarkTheme()
    val colors = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> darkColorScheme()
        else -> lightColorScheme()
    }
    MaterialExpressiveTheme(colorScheme = colors, motionScheme = MotionScheme.expressive(), content = content)
}

@Composable
fun BackButton(onClick: () -> Unit) {
    IconButton(onClick = onClick) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回") }
}

/** Messages and questions (Prompt), on top of every page. */
@Composable
fun PromptDialog(state: AppState) {
    val p = state.prompt ?: return
    val close = { state.prompt = null }
    AlertDialog(
        onDismissRequest = close,
        title = { Text(p.title) },
        text = {
            SelectionContainer {
                Text(p.text, modifier = Modifier.verticalScroll(rememberScrollState()), style = MaterialTheme.typography.bodyMedium)
            }
        },
        confirmButton = {
            TextButton(
                onClick = { close(); p.onConfirm() },
                colors = if (p.destructive) ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error) else ButtonDefaults.textButtonColors(),
            ) { Text(p.confirm) }
        },
        dismissButton = if (p.dismiss == null && p.neutral == null) null else {
            {
                Row {
                    if (p.neutral != null) TextButton(onClick = { close(); p.onNeutral() }) { Text(p.neutral) }
                    if (p.dismiss != null) TextButton(onClick = close) { Text(p.dismiss) }
                }
            }
        },
    )
}

/** The name shown for a mod (AppState.renaming); blank goes back to the Mods entry name. */
@Composable
fun RenameDialog(state: AppState) {
    val name = state.renaming ?: return
    var text by remember(name) { mutableStateOf(state.label(name)) }
    val close = { state.renaming = null }
    AlertDialog(
        onDismissRequest = close,
        title = { Text("重命名") },
        text = {
            OutlinedTextField(
                value = text,
                onValueChange = { if (it.length <= Labels.MAX) text = it.replace("\n", " ") },
                singleLine = true,
                label = { Text("名称") },
                supportingText = { Text("只改 GIBox 里显示的名字；留空恢复为「$name」") },
            )
        },
        confirmButton = { TextButton(onClick = { close(); state.rename(name, text) }) { Text("保存") } },
        dismissButton = { TextButton(onClick = close) { Text("取消") } },
    )
}

/** Progress of background work: an expressive loading indicator that cannot be dismissed. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun BusyDialog(state: AppState) {
    val b = state.busy ?: return
    AlertDialog(
        onDismissRequest = {},
        properties = DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false),
        icon = { ContainedLoadingIndicator(modifier = Modifier.size(56.dp)) },
        title = { Text(b.title) },
        text = { Text(b.text, style = MaterialTheme.typography.bodyMedium) },
        confirmButton = {},
    )
}

/** The title of a SettingsGroup (the group adds the padding). */
@Composable
fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
}

/** A full-width row for an empty list. */
@Composable
fun EmptyNote(text: String) {
    Column(Modifier.fillMaxWidth().heightIn(min = 96.dp).padding(24.dp), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.size(4.dp))
    }
}
