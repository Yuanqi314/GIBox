package dev.modelswap.only

import android.content.ContentResolver
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.FileOutputStream

/** A .zip picked in the system file manager (ACTION_OPEN_DOCUMENT): copied to the cache and unpacked with the usual guards. */
class SafSource(
    private val resolver: ContentResolver,
    private val uri: Uri,
    private val cache: File,
) : Importer.Source {
    private val displayName: String by lazy { queryName() }

    override fun name(): String = displayName

    override fun copyTo(dir: File, log: StringBuilder) {
        val zip = File(cache, "import-" + System.nanoTime() + ".zip")
        try {
            val input = resolver.openInputStream(uri) ?: throw IllegalStateException("无法读取所选文件")
            input.use { FileOutputStream(zip).use { out -> it.copyTo(out) } }
            ModLibrary.unzip(zip, dir)
            log.append("已解压：").append(displayName).append('\n')
        } finally {
            zip.delete()
        }
    }

    private fun queryName(): String {
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val name = c.getString(0)
                if (!name.isNullOrBlank()) return name
            }
        }
        return (uri.lastPathSegment ?: "mod").substringAfterLast('/').substringAfterLast(':').ifBlank { "mod" }
    }
}
