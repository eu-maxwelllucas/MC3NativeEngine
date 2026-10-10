package com.example.mc3native

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.io.FileNotFoundException

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var glView: GLSurfaceView

    external fun inspectIso(fd: Int): String

    companion object {
        init { System.loadLibrary("mc3native") }
        private const val PICK_ISO = 1001
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(2)
            setRenderer(DiagnosticRenderer())
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }

        status = TextView(this).apply {
            text = "MC3 Native Engine 0.1\nRenderizador OpenGL ES 2.0: diagnóstico inicial"
            textSize = 14f
            setTextColor(0xFFFFFFFF.toInt())
            setPadding(18, 12, 18, 12)
        }

        val pick = Button(this).apply {
            text = "Selecionar ISO do PS2"
            setOnClickListener { chooseIso() }
        }

        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.TOP
            setPadding(12, 12, 12, 12)
            setBackgroundColor(0xCC101820.toInt())
            addView(pick)
            addView(status)
        }

        val root = android.widget.FrameLayout(this).apply {
            addView(glView, android.widget.FrameLayout.LayoutParams(-1, -1))
            addView(panel, android.widget.FrameLayout.LayoutParams(-1, -2, Gravity.TOP))
        }
        setContentView(root)
    }

    private fun chooseIso() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(intent, PICK_ISO)
    }

   
@Deprecated("Use of startActivityForResult for broad device compatibility")
override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
    super.onActivityResult(requestCode, resultCode, data)

    if (requestCode != PICK_ISO || resultCode != RESULT_OK) return

    val uri: Uri = data?.data ?: return

    try {
        val pfd = contentResolver.openFileDescriptor(uri, "r")
            ?: throw FileNotFoundException(
                "Não foi possível abrir o arquivo selecionado"
            )

        val fd = pfd.detachFd()

        val result = try {
            inspectIso(fd)
        } finally {
            ParcelFileDescriptor.adoptFd(fd).use { }
        }

        status.text = result
    } catch (e: Exception) {
        status.text = "Falha ao ler ISO: ${e.message ?: e.javaClass.simpleName}"
    }
 }
}
