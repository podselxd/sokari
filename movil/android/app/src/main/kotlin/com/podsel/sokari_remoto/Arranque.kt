package com.podsel.sokari_remoto

import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Cómo arrancó la app y con qué modo de dibujo.
 *
 * Flutter deja la pantalla de arranque de Android (el ícono de Sokari) hasta
 * que dibuja su primer cuadro. Si en un celular nunca lo dibuja (su chip de
 * gráficos no se lleva con el modo normal), la app se quedaba en el ícono sin
 * decir nada. Aquí se recuerda eso y la siguiente vez se prueba otro modo:
 * 0 normal (lo que Flutter elija), 1 Impeller con OpenGL y 2 Skia.
 * Los errores de Android que la tumban se guardan para mostrarlos después.
 */
object Arranque {
    private const val TAG = "Sokari"
    private const val PREFS = "sokari_arranque"
    private val MODOS = arrayOf("normal", "Impeller con OpenGL", "Skia")

    /** Para probar un modo desde afuera (la prueba del emulador): `--ei modo_dibujo 2`. */
    const val EXTRA_MODO = "modo_dibujo"

    private fun prefs(c: Context) = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun archivo(c: Context) = File(c.filesDir, "ultimo_error.txt")

    @Suppress("DEPRECATION")
    private fun version(c: Context): String =
        try {
            val info = c.packageManager.getPackageInfo(c.packageName, 0)
            val codigo = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
            "${info.versionName} ($codigo)"
        } catch (e: Exception) {
            "?"
        }

    /** Al abrir, antes de que arranque Flutter: con qué modo se dibuja esta vez. */
    fun empezar(c: Context, intent: Intent?) {
        val p = prefs(c)
        val e = p.edit()
        val v = version(c)
        if (p.getString("version", null) != v) {
            // Otra versión de la app: se vuelve a probar el modo normal.
            e.putString("version", v).putInt("modo", 0).putBoolean("arrancando", false)
        } else if (p.getBoolean("arrancando", false)) {
            // La vez pasada se cerró (o la cerraron) sin haber dibujado.
            val modo = p.getInt("modo", 0).coerceIn(0, MODOS.size - 1)
            anotar(c, "La vez pasada no alcanzó a mostrar su pantalla (modo de dibujo: ${MODOS[modo]}).")
            e.putInt("modo", (modo + 1).coerceAtMost(MODOS.size - 1))
        }
        val forzado = intent?.getIntExtra(EXTRA_MODO, -1) ?: -1
        if (forzado in MODOS.indices) e.putInt("forzado", forzado) else e.remove("forzado")
        // commit y no apply: tiene que quedar escrito aunque la app truene enseguida
        e.putBoolean("arrancando", true).commit()
    }

    fun modo(c: Context): Int {
        val p = prefs(c)
        val forzado = p.getInt("forzado", -1)
        return if (forzado in MODOS.indices) forzado else p.getInt("modo", 0).coerceIn(0, MODOS.size - 1)
    }

    fun nombreModo(c: Context): String = MODOS[modo(c)]

    /** Lo que se le pasa al motor de Flutter para el modo de esta vez. */
    fun argumentos(c: Context): List<String> =
        when (modo(c)) {
            1 -> listOf("--impeller-backend=opengles")
            2 -> listOf("--enable-impeller=false")
            else -> emptyList()
        }

    /** Ya dibujó: ese modo sirve en este celular y se queda. */
    fun dibujo(c: Context) {
        prefs(c).edit().putBoolean("arrancando", false).commit()
        Log.i(TAG, "primera pantalla dibujada (modo de dibujo: ${nombreModo(c)})")
    }

    /** Te saliste antes de que dibujara: no cuenta como falla la próxima vez. */
    fun salioSinDibujar(c: Context) {
        prefs(c).edit().putBoolean("arrancando", false).apply()
    }

    /** No dibujó a tiempo: se anota y la próxima vez prueba el siguiente modo. Devuelve el reporte. */
    fun noDibujo(c: Context, segundos: Int): String {
        val p = prefs(c)
        anotar(c, "No alcanzó a mostrar su pantalla en $segundos s (modo de dibujo: ${nombreModo(c)}).")
        val siguiente = (p.getInt("modo", 0) + 1).coerceAtMost(MODOS.size - 1)
        // "arrancando" queda en false: esta falla ya se contó, no se cuenta otra vez al abrir
        p.edit().putInt("modo", siguiente).remove("forzado").putBoolean("arrancando", false).commit()
        return (leer(c) ?: "") + "\n\n" + celular(c)
    }

    /** Un error de Android que tumbe la app se guarda antes de cerrarse. */
    fun vigilarErrores(c: Context) {
        val anterior = Thread.getDefaultUncaughtExceptionHandler()
        if (anterior is Vigia) return
        Thread.setDefaultUncaughtExceptionHandler(Vigia(c.applicationContext, anterior))
    }

    private class Vigia(
        private val c: Context,
        private val anterior: Thread.UncaughtExceptionHandler?,
    ) : Thread.UncaughtExceptionHandler {
        override fun uncaughtException(t: Thread, e: Throwable) {
            try {
                val sw = StringWriter()
                e.printStackTrace(PrintWriter(sw))
                anotar(c, "Se cerró por un error de Android (hilo ${t.name}):\n" + sw.toString().take(4000))
            } catch (_: Throwable) {
            }
            anterior?.uncaughtException(t, e)
        }
    }

    fun anotar(c: Context, texto: String) {
        try {
            val f = archivo(c)
            val previo = if (f.exists()) f.readText().takeLast(6000) else ""
            val hora = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
            f.writeText(previo + (if (previo.isEmpty()) "" else "\n\n") + "[$hora] $texto")
            Log.w(TAG, texto)
        } catch (_: Throwable) {
        }
    }

    private fun leer(c: Context): String? =
        try {
            archivo(c).takeIf { it.exists() }?.readText()?.takeIf { it.isNotBlank() }
        } catch (_: Throwable) {
            null
        }

    /** Lo que se guardó (y se borra: se muestra una sola vez), con los datos del celular. */
    fun tomarError(c: Context): String? {
        val t = leer(c) ?: return null
        archivo(c).delete()
        return t + "\n\n" + celular(c)
    }

    fun celular(c: Context): String =
        "Celular: ${Build.MANUFACTURER} ${Build.MODEL} · Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}) · " +
            "${Build.SUPPORTED_ABIS.joinToString()} · app ${version(c)} · dibujo: ${nombreModo(c)}"
}
