package com.podsel.sokari_remoto

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.lang.ref.WeakReference

/**
 * El aviso cuando Flutter no alcanzó a dibujar: una pantalla de Android sola
 * (sin Flutter), así se ve aunque el dibujo de Flutter no funcione en este
 * celular. «Reintentar» vuelve a abrir la app, ya con otro modo de dibujo.
 */
class AvisoActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        visible = WeakReference(this)
        val texto = intent.getStringExtra(TEXTO) ?: ""
        val d = resources.displayMetrics.density
        val margen = (20 * d).toInt()

        // afuera respeta la barra de estado y la de navegación; adentro, el margen
        val marco = FrameLayout(this).apply {
            setBackgroundColor(Color.rgb(0x14, 0x12, 0x1B))
            fitsSystemWindows = true
        }
        val raiz = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(margen, margen, margen, margen)
        }
        marco.addView(raiz, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        raiz.addView(TextView(this).apply {
            text = "Sokari no pudo mostrar su pantalla"
            textSize = 20f
            setTextColor(Color.WHITE)
            setTypeface(typeface, Typeface.BOLD)
        })
        raiz.addView(TextView(this).apply {
            text = "Toca «Reintentar»: la app se vuelve a abrir con otro modo de dibujo. Si sigue igual, toca " +
                "«Copiar» y manda este texto a quien te ayuda con Sokari."
            textSize = 15f
            setTextColor(Color.rgb(0xDD, 0xDA, 0xE8))
            setPadding(0, margen / 2, 0, margen / 2)
        })
        val detalle = TextView(this).apply {
            this.text = texto
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setTextColor(Color.rgb(0xB8, 0xB4, 0xC8))
            setTextIsSelectable(true)
        }
        raiz.addView(
            ScrollView(this).apply { addView(detalle) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f),
        )
        val botones = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END
        }
        botones.addView(Button(this).apply {
            text = "Copiar"
            setOnClickListener { copiar(texto) }
        })
        botones.addView(Button(this).apply {
            text = "Reintentar"
            setOnClickListener { reiniciar() }
        })
        raiz.addView(botones)
        setContentView(marco)
    }

    override fun onDestroy() {
        if (visible?.get() === this) visible = null
        super.onDestroy()
    }

    private fun copiar(texto: String) {
        val portapapeles = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        portapapeles.setPrimaryClip(ClipData.newPlainText("Sokari", texto))
        Toast.makeText(this, "Copiado", Toast.LENGTH_SHORT).show()
    }

    /** El modo de dibujo se elige al arrancar el proceso: hay que cerrarlo y abrirlo de nuevo. */
    private fun reiniciar() {
        val abrir = packageManager.getLaunchIntentForPackage(packageName) ?: return
        startActivity(Intent.makeRestartActivityTask(abrir.component))
        Runtime.getRuntime().exit(0)
    }

    companion object {
        const val TEXTO = "texto"
        private var visible: WeakReference<AvisoActivity>? = null

        /** Si Flutter terminó dibujando (un celular lento), el aviso sobra. */
        fun cerrarSiSeVe() {
            visible?.get()?.finish()
        }
    }
}
