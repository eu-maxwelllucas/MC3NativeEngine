package com.example.mc3native

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {

    private lateinit var statusText: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
        }

        val scrollView = ScrollView(this)
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        val title = TextView(this).apply {
            text = "MC3 Native Engine"
            textSize = 26f
        }

        val description = TextView(this).apply {
            text = """
                Diagnóstico experimental do Midnight Club 3: DUB Edition Remix.

                Selecione uma ISO de PS2 para analisar seus arquivos ou execute o teste gráfico OpenGL ES.
            """.trimIndent()
            textSize = 16f
        }

        val selectButton = Button(this).apply {
            text = "Selecionar ISO do PS2"
            setOnClickListener {
                selectIso()
            }
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

        statusText = TextView(this).apply {
            text = "Aguardando seleção de ISO."
            textSize = 15f
        }

        content.addView(
            title,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        )

        content.addView(
            description,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = 16
            }
        )

        content.addView(
            selectButton,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = 20
            }
        )

        content.addView(
            graphicsButton,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = 8
            }
        )

        content.addView(
            statusText,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = 20
            }
        )

        scrollView.addView(content)
        root.addView(
            scrollView,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )

        setContentView(root)
    }

    private fun selectIso() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }

        @Suppress("DEPRECATION")
        startActivityForResult(intent, REQUEST_SELECT_ISO)
    }

    @Deprecated("Usado para manter compatibilidade com o fluxo existente")
    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?
    ) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode == REQUEST_SELECT_ISO &&
            resultCode == RESULT_OK
        ) {
            val uri = data?.data ?: return

            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )

            statusText.text =
                "ISO selecionada:\n$uri\n\n" +
                "A integração com o analisador de ISO precisa ser conectada aqui."
        }
    }

    companion object {
        private const val REQUEST_SELECT_ISO = 1001
    }
}
