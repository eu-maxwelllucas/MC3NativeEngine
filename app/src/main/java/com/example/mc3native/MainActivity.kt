
package com.example.mc3native

import android.app.Activity
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.content.Intent
import android.net.Uri
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.graphics.Color
import android.opengl.GLSurfaceView
import java.io.FileNotFoundException
import java.io.IOException
import java.nio.charset.StandardCharsets
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class MainActivity : Activity() {

    companion object {
        private const val PICK_ISO = 1001
        private const val SAVE_LOG = 1002
    }

    private lateinit var status: TextView
    private var pendingReport: String = ""

    external fun inspectIso(fd: Int): String

    init {
        System.loadLibrary("native-lib")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(24, 24, 24, 24)
            setBackgroundColor(Color.rgb(18, 18, 24))
        }

        val title = TextView(this).apply {
            text = "MC3 Native Engine"
            textSize = 25f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        }

        val subtitle = TextView(this).apply {
            text = "Diagnóstico de imagem PS2"
            textSize = 16f
            setTextColor(Color.LTGRAY)
            gravity = Gravity.CENTER
        }

        val glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(2)
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(
                    gl: GL10?,
                    config: EGLConfig?
                ) {
                    android.opengl.GLES20.glClearColor(
                        0.07f, 0.07f, 0.10f, 1f
                    )
                }

                override fun onSurfaceChanged(
                    gl: GL10?,
                    width: Int,
                    height: Int
                ) {
                    android.opengl.GLES20.glViewport(
                        0, 0, width, height
                    )
                }

                override fun onDrawFrame(gl: GL10?) {
                    android.opengl.GLES20.glClear(
                        android.opengl.GLES20.GL_COLOR_BUFFER_BIT
                    )
                }
            })
        }

        val selectButton = Button(this).apply {
            text = "Selecionar ISO do PS2"
            setOnClickListener {
                selectIso()
            }
        }

        status = TextView(this).apply {
            text = "Selecione uma ISO para iniciar o diagnóstico."
            textSize = 15f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        }

        root.addView(
            title,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        )

        root.addView(
            subtitle,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = 8
                bottomMargin = 16
            }
        )

        root.addView(
            glView,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        )

        root.addView(
            selectButton,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = 16
                bottomMargin = 12
            }
        )

        root.addView(
            status,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        )

        setContentView(root)
    }

    private fun selectIso() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }

        startActivityForResult(intent, PICK_ISO)
    }

    @Deprecated("Compatibilidade com a implementação atual do projeto")
    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?
    ) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode == PICK_ISO) {
            if (resultCode != RESULT_OK) {
                status.text = "Seleção da ISO cancelada."
                return
            }

            val uri: Uri = data?.data ?: run {
                status.text = "Nenhum arquivo foi selecionado."
                return
            }

            status.text = "Analisando ISO... Aguarde."

            try {
                val pfd = contentResolver.openFileDescriptor(uri, "r")
                    ?: throw FileNotFoundException(
                        "Não foi possível abrir o arquivo selecionado."
                    )

                val fd = pfd.detachFd()

                pendingReport = try {
                    inspectIso(fd)
                } finally {
                    ParcelFileDescriptor.adoptFd(fd).use { }
                }

                val saveIntent = Intent(
                    Intent.ACTION_CREATE_DOCUMENT
                ).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "text/plain"
                    putExtra(
                        Intent.EXTRA_TITLE,
                        "diagnostico.txt"
                    )
                }

                status.text = "Análise concluída. Escolha onde salvar."
                startActivityForResult(saveIntent, SAVE_LOG)

            } catch (e: Exception) {
                status.text =
                    "Falha na análise: ${e.message ?: e.javaClass.simpleName}"
            }

            return
        }

        if (requestCode == SAVE_LOG) {
            if (resultCode != RESULT_OK) {
                status.text = "Salvamento cancelado. O relatório não foi gravado."
                return
            }

            val uri: Uri = data?.data ?: run {
                status.text = "Destino do arquivo não identificado."
                return
            }

            try {
                val output = contentResolver.openOutputStream(uri, "wt")
                    ?: throw IOException(
                        "Não foi possível criar o arquivo de relatório."
                    )

                output.use {
                    it.write(
                        pendingReport.toByteArray(
                            StandardCharsets.UTF_8
                        )
                    )
                    it.flush()
                }

                status.text =
                    "Relatório salvo!\nAbra diagnostico.txt para ver os detalhes."

            } catch (e: Exception) {
                status.text =
                    "Erro ao salvar: ${e.message ?: e.javaClass.simpleName}"
            }
        }
    }
}
