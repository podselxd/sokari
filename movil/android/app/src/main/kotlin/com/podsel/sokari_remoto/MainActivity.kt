// FlutterShellArgs está marcado como obsoleto, pero es la única forma de elegir
// el modo de dibujo en cada arranque (en el manifiesto es fijo para siempre).
@file:Suppress("DEPRECATION")

package com.podsel.sokari_remoto

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterShellArgs
import io.flutter.plugin.common.MethodChannel

/**
 * La pantalla de la app (Flutter), con red de seguridad para el arranque:
 * si en 10 s no dibuja su primer cuadro, en vez de quedarse en el ícono de
 * Sokari abre un aviso de Android (no depende de Flutter) con el detalle, y
 * la siguiente vez prueba otro modo de dibujo (ver Arranque).
 */
class MainActivity : FlutterActivity() {
    private val principal = Handler(Looper.getMainLooper())

    @Volatile private var dibujo = false

    /** Solo la prueba del emulador: el aviso como si no hubiera dibujado. */
    private var probarAviso = false

    private val vigilarDibujo = Runnable { if (!dibujo && !isFinishing) noDibujo() }

    override fun onCreate(savedInstanceState: Bundle?) {
        Arranque.vigilarErrores(this)
        Arranque.empezar(this, intent)
        probarAviso = intent?.getBooleanExtra(EXTRA_PROBAR_AVISO, false) == true
        super.onCreate(savedInstanceState)
    }

    // Se vigila solo con la app a la vista: si te sales antes de que dibuje,
    // no es una falla (y Android no deja abrir el aviso desde el fondo).
    override fun onResume() {
        super.onResume()
        if (!dibujo && !probarAviso) principal.postDelayed(vigilarDibujo, ESPERA_MS)
    }

    override fun onPause() {
        principal.removeCallbacks(vigilarDibujo)
        if (!dibujo) Arranque.salioSinDibujar(this)
        super.onPause()
    }

    override fun onDestroy() {
        principal.removeCallbacks(vigilarDibujo)
        super.onDestroy()
    }

    override fun getFlutterShellArgs(): FlutterShellArgs {
        val args = super.getFlutterShellArgs()
        for (a in Arranque.argumentos(this)) args.add(a)
        return args
    }

    override fun onFlutterUiDisplayed() {
        super.onFlutterUiDisplayed()
        dibujo = true
        principal.removeCallbacks(vigilarDibujo)
        Arranque.dibujo(this)
        if (probarAviso) {
            probarAviso = false
            principal.postDelayed({ noDibujo() }, 1000)
        } else {
            AvisoActivity.cerrarSiSeVe()
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "sokari/arranque").setMethodCallHandler { call, result ->
            when (call.method) {
                "ultimoError" -> result.success(Arranque.tomarError(this))
                "celular" -> result.success(Arranque.celular(this))
                else -> result.notImplemented()
            }
        }
    }

    private fun noDibujo() {
        val reporte = Arranque.noDibujo(this, (ESPERA_MS / 1000).toInt())
        startActivity(Intent(this, AvisoActivity::class.java).putExtra(AvisoActivity.TEXTO, reporte))
    }

    private companion object {
        const val ESPERA_MS = 10_000L
        const val EXTRA_PROBAR_AVISO = "probar_aviso"
    }
}
