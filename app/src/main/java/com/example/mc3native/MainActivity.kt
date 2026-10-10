package com.example.mc3native

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.IOException
import java.nio.charset.StandardCharsets

class MainActivity : Activity() {

    companion object {
        private const val PICK_ISO = 1001
        private const val SAVE_LOG = 1002
    }

    private lateinit var status: TextView
    private var pendingReport = ""
    private var nativeLoaded = false

    private external fun inspectIso(fd: Int): String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(24, 24, 24, 24)
            setBackgroundColor(Color.rgb(24, 24, 32))
        }

        val title = TextView(this).apply {
            text = "MC3 Native Engine"
            textSize = 25f
            gravity = Gravity.CENTER
            setTextColor(Color.WHITE)
        }

        val description = TextView(this).apply {
            text = "Diagnóstico da ISO do PlayStation 2"
            textSize = 16f
            gravity = Gravity.CENTER
            setTextColor(Color.LTGRAY)
        }

        val selectButton = Button(this).apply {
            text = "Selecionar ISO do PS2"
            setOnClickListener { selectIso() }
        }

        val graphicsButton = Button(this).apply {
            text = "Teste gráfico OpenGL ES"
            setOnClickListener {
                startActivity(
                    Intent(
                        this@MainActivity,
                        DiagnosticActivity::class.java
                    )
                )
            }
        }

        status = TextView(this).apply {
            text = "Aplicativo iniciado.\nSelecione a ISO para analisar."
            textSize = 16f
            gravity = Gravity.CENTER
            setTextColor(Color.WHITE)
        }

        root.addView(title)
        root.addView(description)
        root.addView(selectButton)
        root.addView(graphicsButton)
        root.addView(status)

        setContentView(
            ScrollView(this).apply {
                addView(root)
                isFillViewport = true
            }
        )
    }

    private fun selectIso() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(intent, PICK_ISO)
    }

    @Deprecated("Mantido para compatibilidade")
    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?
    ) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode == PICK_ISO) {
            if (resultCode != RESULT_OK) {
                status.text = "Seleção cancelada."
                return
            }

            val uri = data?.data ?: return
            analyzeIso(uri)
            return
        }

        if (requestCode == SAVE_LOG) {
            if (resultCode != RESULT_OK) {
                status.text = "Salvamento cancelado."
                return
            }

            val uri = data?.data ?: return
            saveReport(uri)
        }
    }

    private fun analyzeIso(uri: Uri) {
        status.text = "Analisando ISO. Aguarde..."

        try {
            if (!nativeLoaded) {
                System.loadLibrary("native-lib")
                nativeLoaded = true
            }

            val pfd = contentResolver.openFileDescriptor(uri, "r")
                ?: throw IOException("Não foi possível abrir a ISO.")

            pendingReport = pfd.use { descriptor ->
                inspectIso(descriptor.fd)
            }

            val saveIntent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "text/plain"
                putExtra(Intent.EXTRA_TITLE, "diagnostico.txt")
            }

            status.text = "Análise concluída. Escolha onde salvar."
            startActivityForResult(saveIntent, SAVE_LOG)

        } catch (e: Exception) {
            status.text =
                "Falha na análise:\n${e.message ?: e.javaClass.simpleName}"
        } catch (e: UnsatisfiedLinkError) {
            status.text =
                "Erro ao carregar a biblioteca nativa:\n${e.message}"
        }
    }

    private fun saveReport(uri: Uri) {
        try {
            val output = contentResolver.openOutputStream(uri, "wt")
                ?: throw IOException("Não foi possível criar o arquivo.")

            output.use {
                it.write(pendingReport.toByteArray(StandardCharsets.UTF_8))
            }

            status.text = "Relatório salvo com sucesso!"

        } catch (e: Exception) {
            status.text =
                "Falha ao salvar:\n${e.message ?: e.javaClass.simpleName}"
        }
    }
}
