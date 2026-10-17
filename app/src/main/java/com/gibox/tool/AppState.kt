package com.gibox.tool

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.widget.Toast
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File
import kotlin.concurrent.thread

const val VERSION = "0.7.2"
const val BUILD_ID = "2b4dd73155e845a2ef698ec500509baa4eb3bfa8"
private const val LOGGER = "importer"

/** Pages; the app keeps a back stack of them (system back pops it). */
sealed interface Screen {
    data object Home : Screen
    data object Settings : Screen
    data object Log : Screen
    data object Manual : Screen
    /** The mods of one character (id 0: mods without a chosen character). */
    data class Character(val id: Int) : Screen
    data class Detail(val name: String) : Screen
    /**
     * Character choice. For a mod being imported, `pending` is its copy in Mods/.pending (dropped when the page is left
     * without 确定); for an imported mod, `source` is its folder in Mods.
     */
    data class Pick(val modName: String, val importedAs: String, val pending: File?, val source: File?, val current: Int) : Screen
}

/** A message or a question. `neutral` is a third choice (另存). */
class Prompt(
    val title: String,
    val text: String,
    val confirm: String = "确定",
    val onConfirm: () -> Unit = {},
    val dismiss: String? = null,
    val neutral: String? = null,
    val onNeutral: () -> Unit = {},
    val destructive: Boolean = false,
)

class Busy(val title: String, val text: String)

/** Everything the pages show and do. File work runs on a background thread; state is changed on the UI thread only. */
class AppState(private val activity: Activity) {
    var game by mutableStateOf(readPrivate("game.txt").takeIf { Control.supported(it) } ?: "com.miHoYo.Yuanshen")
        private set
    var mods by mutableStateOf<List<Importer.Mod>>(emptyList())
        private set
    var status by mutableStateOf(Control.Status())
        private set
    var enabled by mutableStateOf(false)
        private set
    var storage by mutableStateOf(false)
        private set
    /** LSPosed loaded GIBox into its own process (ModuleStatus, set by Entry). */
    var moduleActive by mutableStateOf(false)
        private set
    var frameworkApi by mutableStateOf(0)
        private set
    var logLevel by mutableStateOf("info")
        private set
    var busy by mutableStateOf<Busy?>(null)
        private set
    var prompt by mutableStateOf<Prompt?>(null)
    /** Mods entry whose name is being edited (RenameDialog). */
    var renaming by mutableStateOf<String?>(null)
    /** Names given to mods (Labels), by Mods entry name. */
    var labels by mutableStateOf<Map<String, String>>(emptyMap())
        private set
    /** Bumped when control.json changes (variants, selection), so pages re-read it. */
    var revision by mutableStateOf(0)
        private set
    val stack = mutableStateListOf<Screen>(Screen.Home)
    /** Servers whose old folders were already looked at this run (declared before init, which refreshes). */
    private val migrationChecked = mutableSetOf<String>()

    /** Imported mods: GIBox's private storage. Models, skeletons, control/status and the log: the game's media folder. */
    val modsRoot: File get() = Control.mods(activity.filesDir, game)
    val modelsRoot: File get() = Control.models(game)
    val boneRoot: File get() = Control.bones(game)
    val logFile: File get() = AppLog.file(game)
    private val labelsFile: File get() = File(activity.filesDir, "labels/$game.json")

    init {
        AppLog.debug(game, "app", "App Start $VERSION")
        Importer.discardAll(modsRoot)
        refresh()
    }

    // ---------------------------------------------------------------- navigation
    fun open(screen: Screen) { stack.add(screen) }
    fun back() {
        if (stack.size <= 1) return
        val top = stack.removeAt(stack.lastIndex)
        if (top is Screen.Pick && top.pending != null) {
            Importer.discard(top.pending)
            AppLog.info(game, LOGGER, "取消导入 ${top.modName}")
        }
    }
    private fun closeTop() { if (stack.size > 1) stack.removeAt(stack.lastIndex) }

    // ---------------------------------------------------------------- state
    fun refresh() {
        storage = hasStorage()
        if (storage && busy == null && game !in migrationChecked) migrate()
        moduleActive = ModuleStatus.active
        frameworkApi = ModuleStatus.api
        mods = Importer.list(modsRoot, modelsRoot)
        labels = Labels.read(labelsFile)
        enabled = Control.enabled(game)
        logLevel = Control.logLevel(game)
        status = Control.status(game)
        revision++
    }
    fun refreshStatus() { status = Control.status(game) }

    // ---------------------------------------------------------------- 0.5 → 0.6
    /** Moves mods and control.json out of the old folders once per server (Migration), then offers to convert the mods again. */
    private fun migrate() {
        val pkg = game
        migrationChecked.add(pkg)
        val legacy = Migration.legacy(pkg)
        if (!Migration.needed(legacy, Control.shared(pkg))) return
        work("迁移旧版数据", "正在把已导入的 MOD 移到 GIBox 自己的目录…", { Migration.run(legacy, Control.shared(pkg), Control.mods(activity.filesDir, pkg)) }) { r ->
            AppLog.info(pkg, "app", "迁移：${r.mods.size} 个 MOD" + (if (r.control) "，control.json" else "") + (if (r.errors.isEmpty()) "" else "；失败：" + r.errors.joinToString("；")))
            refresh()
            val failed = if (r.errors.isEmpty()) "" else "\n\n没能移动：\n" + r.errors.joinToString("\n")
            prompt = if (r.mods.isEmpty()) Prompt("已迁移旧版数据", "开关、勾选和款式设置已沿用。$failed")
            else Prompt(
                title = "已迁移旧版数据",
                text = "${r.mods.size} 个已导入的 MOD 移到了 GIBox 自己的目录。\n模型换成了 GIBox 自己的格式，旧模型不再读取，需要重新转换（用导入时选的角色）。$failed",
                confirm = "全部转换", onConfirm = { convertAll(r.mods.map { it.name }) }, dismiss = "稍后",
            )
        }
    }
    /** Re-converts the named mods for the character recorded in each; mods without one are left for the character page. */
    private fun convertAll(names: List<String>) {
        work("转换", "正在转换 ${names.size} 个 MOD…", { log ->
            val done = mutableListOf<String>()
            val skipped = mutableListOf<String>()
            val failed = mutableListOf<String>()
            for (m in Importer.list(modsRoot, modelsRoot)) {
                if (m.source == null || m.sourceName !in names) continue
                if (m.id == 0) { skipped.add(m.name); continue }
                val one = StringBuilder()
                try { Importer.convert(m.source, m.id, modelsRoot, boneRoot, decoder, one); done.add(m.name) }
                catch (e: Throwable) { failed.add("${m.name}：${e.message ?: e}") }
                AppLog.debug(game, "converter", one.toString().trim())
            }
            log.append("完成 ${done.size} 个")
            Triple(done, skipped, failed)
        }) { (done, skipped, failed) ->
            AppLog.info(game, LOGGER, "全部转换：完成 ${done.size}，未选角色 ${skipped.size}，失败 ${failed.size}")
            refresh()
            val text = StringBuilder("已转换 ${done.size} 个。")
            if (skipped.isNotEmpty()) text.append("\n\n还没选角色（在 MOD 详情里选）：\n").append(skipped.joinToString("\n"))
            if (failed.isNotEmpty()) text.append("\n\n失败：\n").append(failed.joinToString("\n"))
            prompt = Prompt("全部转换", text.toString())
        }
    }
    fun mod(name: String): Importer.Mod? = mods.firstOrNull { it.name == name }
    /** The name shown for a mod: the one given to it, else its Mods entry name. */
    fun label(name: String): String = Labels.of(labels, name)

    /** One row of the home list: a character and its mods. */
    class Group(val id: Int, val title: String, val mods: List<Importer.Mod>)
    /** Mods by character, in character ID order; mods without a chosen character last. */
    fun groups(): List<Group> = mods.groupBy { it.id }.toSortedMap(compareBy { if (it == 0) Int.MAX_VALUE else it })
        .map { (id, list) -> Group(id, if (id == 0) "未选择角色" else Characters.nameOf(id), list.sortedBy { label(it.name).lowercase() }) }
    fun group(id: Int): Group? = groups().firstOrNull { it.id == id }

    fun rename(name: String, text: String) = save("改名") {
        Labels.set(labelsFile, name, text)
        AppLog.info(game, LOGGER, "改名 $name → ${text.trim().ifEmpty { name }}")
    }
    fun selected(name: String): Boolean = Control.selected(game, name)

    fun setServer(pkg: String) {
        game = pkg
        writePrivate("game.txt", pkg)
        Importer.discardAll(modsRoot)
        refresh()
    }
    fun setReplacement(on: Boolean) = save("保存") {
        Control.setEnabled(game, on)
        AppLog.info(game, "app", if (on) "模型替换已开启" else "模型替换已关闭")
    }
    /** Switching on a second mod of a character asks first (both, only this one, or not). */
    fun setSelected(mod: Importer.Mod, on: Boolean) {
        if (!on) { save("保存") { Control.disable(game, mod.name); AppLog.info(game, LOGGER, "停用 ${mod.name}") }; return }
        val others = enabledWith(mod.name, mod.id)
        if (others.isEmpty()) { save("保存") { enable(mod.name, emptyList()) }; return }
        prompt = bothPrompt("同时开启多个 MOD？", "", mod.name, mod.id, others, "取消")
    }
    /** Other converted, enabled mods of the same character. */
    private fun enabledWith(name: String, id: Int): List<String> =
        if (id == 0) emptyList() else mods.filter { it.name != name && it.id == id && it.converted && Control.selected(game, it.name) }.map { it.name }
    private fun enable(name: String, off: List<String>) {
        for (o in off) Control.disable(game, o)
        Control.enable(game, name)
        AppLog.info(game, LOGGER, "启用 $name" + if (off.isEmpty()) "" else "，停用 " + off.joinToString("、"))
    }
    /** "Both / only this one / dismiss" for a mod whose character already has enabled mods. */
    private fun bothPrompt(title: String, lead: String, name: String, id: Int, others: List<String>, dismiss: String) = Prompt(
        title = title,
        text = lead + "${Characters.nameOf(id)} 已经开启了：${others.joinToString("、") { label(it) }}\n" +
            "同时开启时，两个 MOD 改到同一个部件的，以先开启的为准；各改各的部件就都会显示。",
        confirm = "都开启", onConfirm = { save("保存") { enable(name, emptyList()) } },
        neutral = "只开这个", onNeutral = { save("保存") { enable(name, others) } },
        dismiss = dismiss,
    )
    fun setVariable(mod: Importer.Mod, variable: String, value: Int) = save("保存") { Control.setVariable(game, mod.name, variable, value) }
    fun changeLogLevel(level: String) = save("保存") { Control.setLogLevel(game, level) }
    fun exportBones() = save("保存") {
        Control.requestBoneExport(game)
        toast("已通知游戏：当前角色在场时导出骨骼")
    }
    private fun save(what: String, change: () -> Unit) {
        try { change() } catch (e: Throwable) { fail(what, e, null) }
        refresh()
    }

    // ---------------------------------------------------------------- import
    /** A .zip the system file picker returned. */
    fun importFrom(uri: Uri) {
        if (!storage) { needStorage(); return }
        val source = SafSource(activity.contentResolver, uri, activity.cacheDir)
        work("导入 MOD", "正在解压…", { log ->
            val pending = Importer.prepare(source, modsRoot, log)
            AppLog.info(game, LOGGER, "读取 ${source.name()}：${log.toString().trim()}")
            Pair(pending, source.name())
        }) { (pending, picked) ->
            val name = Importer.suggestName(picked)
            val before = mods.firstOrNull { it.name == name }?.id ?: 0
            open(Screen.Pick(name, picked, pending, null, before))
        }
    }
    /** Confirmed on the character page. */
    fun confirm(pick: Screen.Pick, avatar: Characters.Avatar) {
        val pending = pick.pending
        if (pending == null) {
            closeTop()
            pick.source?.let { convert(it, avatar.id) }
            return
        }
        val name = pick.modName
        if (File(modsRoot, name).exists() || File(modelsRoot, name).exists()) {
            val other = Importer.freeName(modsRoot, modelsRoot, name)
            prompt = Prompt(
                title = "已有同名 MOD",
                text = "「$name」已经导入过。\n覆盖：换成这次选的文件\n另存：作为「$other」导入",
                confirm = "覆盖", onConfirm = { commit(pick, pending, name, avatar.id) },
                neutral = "另存", onNeutral = { commit(pick, pending, other, avatar.id) },
                dismiss = "取消",
            )
            return
        }
        commit(pick, pending, name, avatar.id)
    }
    private fun commit(pick: Screen.Pick, pending: File, name: String, id: Int) {
        if (stack.lastOrNull() == pick) closeTop()
        AppLog.info(game, LOGGER, "导入 ${pick.importedAs} → Mods/$name，角色 ${Characters.nameOf(id)}（$id）")
        convertJob(name) { log -> Importer.convert(Importer.commit(pending, modsRoot, name), id, modelsRoot, boneRoot, decoder, log) }
    }
    /** Re-conversion of an imported mod, for `id`. */
    fun convert(source: File, id: Int) {
        AppLog.info(game, LOGGER, "转换 ${source.name} 为 ${Characters.nameOf(id)}（$id）")
        convertJob(source.name) { log -> Importer.convert(source, id, modelsRoot, boneRoot, decoder, log) }
    }
    private fun convertJob(name: String, job: (StringBuilder) -> Importer.Result) {
        work("转换", "正在转换 $name…\n大的 MOD 可能要一分钟。", { log ->
            try { job(log).also { AppLog.debug(game, "converter", log.toString().trim()) } }
            catch (e: Throwable) { AppLog.debug(game, "converter", log.toString().trim()); throw e }
        }) { r ->
            val model = r.model.name
            AppLog.info(game, LOGGER, "完成 $model：${r.summary.replace('\n', ' ')}")
            refresh()
            val others = enabledWith(model, r.id)
            if (others.isEmpty()) {
                try { enable(model, emptyList()) } catch (e: Throwable) { AppLog.warning(game, LOGGER, "启用失败：$e") }
                refresh()
                prompt = Prompt("导入完成", "${label(model)}\n${r.summary}\n\n已启用。游戏运行中也会自动载入。")
            } else {
                prompt = bothPrompt("导入完成", "${label(model)}\n${r.summary}\n\n", model, r.id, others, "先不开启")
            }
        }
    }
    fun guess(pick: Screen.Pick): Characters.Guess =
        Importer.guess(pick.pending ?: pick.source ?: File(modsRoot, pick.modName), pick.importedAs, boneRoot)
    fun recorded(): Set<Int> = ModLibrary.library(boneRoot).map { it.id }.toSet()

    fun delete(mod: Importer.Mod) {
        prompt = Prompt(
            title = "删除 ${label(mod.name)}", text = "删除 Mods 里的这份 MOD 和转换好的模型？导入时选的原文件不受影响。",
            confirm = "删除", dismiss = "取消", destructive = true,
            onConfirm = {
                try { Control.disable(game, mod.name); Importer.delete(mod); Labels.set(labelsFile, mod.name, null); AppLog.info(game, LOGGER, "删除 ${mod.name}") }
                catch (e: Throwable) { fail("删除", e, null) }
                if (stack.lastOrNull() == Screen.Detail(mod.name)) closeTop()
                refresh()
            },
        )
    }

    /** The old manual conversion (settings → 高级): explicit bone folder, ID, name and mapping. */
    fun manualConvert(src: String, bones: String, id: Int, name: String, mapping: String, variants: Boolean) {
        val dest = File(modelsRoot, name)
        if (dest.exists()) { prompt = Prompt("错误", "输出目录已经存在，请换一个模型名：$dest"); return }
        work("手动转换", "正在转换…", { log -> ModelConverter.convert(File(src), File(bones), dest, id, mapping, variants, decoder, log) }) { out ->
            AppLog.info(game, "converter", "手动转换完成：$out")
            refresh()
            prompt = Prompt("转换结束", out.joinToString("\n"))
        }
    }

    // ---------------------------------------------------------------- system
    fun launch() {
        val intent = activity.packageManager.getLaunchIntentForPackage(game)
        if (intent == null) { prompt = Prompt("错误", "没有找到游戏：$game\n（没有安装，或者在设置里选错了服务器）"); return }
        AppLog.info(game, "app", "启动 $game")
        activity.startActivity(intent)
    }
    fun grantStorage() {
        try {
            if (Build.VERSION.SDK_INT >= 30) activity.startActivity(Intent("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION", Uri.parse("package:" + activity.packageName)))
            else activity.requestPermissions(arrayOf("android.permission.READ_EXTERNAL_STORAGE", "android.permission.WRITE_EXTERNAL_STORAGE"), 1)
        } catch (e: Throwable) { fail("打开权限设置", e, null) }
    }
    fun needStorage() {
        prompt = Prompt("需要文件访问权限", "转换好的模型要放进原神的目录（Android/media），游戏才读得到，需要“所有文件访问权限”。", confirm = "去授予", onConfirm = { grantStorage() }, dismiss = "取消")
    }
    private fun hasStorage(): Boolean =
        if (Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager()
        else activity.checkSelfPermission("android.permission.READ_EXTERNAL_STORAGE") == PackageManager.PERMISSION_GRANTED
    fun copy(label: String, text: String) {
        val clipboard = activity.getSystemService(ClipboardManager::class.java) ?: return
        clipboard.setPrimaryClip(ClipData.newPlainText(label, text))
        toast("已复制：$label")
    }
    fun toast(text: String) { Toast.makeText(activity, text, Toast.LENGTH_SHORT).show() }
    fun readLog(): String = AppLog.tail(logFile, 128 * 1024)
    fun clearLog() {
        File(logFile.path + ".1").delete()
        logFile.delete()
    }

    // ---------------------------------------------------------------- background work, errors
    private val decoder = ModelConverter.TextureDecoder { NativeBridge.decodeTexture(it) }
    private fun <T> work(title: String, text: String, job: (StringBuilder) -> T, done: (T) -> Unit) {
        busy = Busy(title, text)
        thread(name = "gibox-work") {
            val log = StringBuilder()
            val result = runCatching { job(log) }
            activity.runOnUiThread {
                busy = null
                if (activity.isFinishing) return@runOnUiThread
                result.fold(onSuccess = { done(it) }, onFailure = { fail(title, it, log) })
            }
        }
    }
    fun fail(what: String, e: Throwable, log: StringBuilder?) {
        AppLog.error(game, LOGGER, "${what}失败：$e")
        val details = if (log != null && log.isNotEmpty()) "\n\n" + log.toString().trim() else ""
        prompt = Prompt("错误", "${what}失败：${e.message ?: e.toString()}$details\n\n日志文件：$logFile")
    }
    private fun readPrivate(name: String): String = try { File(activity.filesDir, name).readText().trim() } catch (e: Exception) { "" }
    private fun writePrivate(name: String, value: String) { try { File(activity.filesDir, name).writeText(value) } catch (e: Exception) { } }
}

/** "茜特菈莉 · 10000107", or what is missing. */
fun who(mod: Importer.Mod): String = when {
    mod.id != 0 -> Characters.nameOf(mod.id) + " · " + mod.id
    mod.converted -> "按名字识别"
    else -> "未选择角色"
}
fun serverName(pkg: String): String = if (pkg == "com.miHoYo.Yuanshen") "官服" else "B 服"
