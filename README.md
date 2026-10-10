# MC3 Native Engine — marco técnico 1

Protótipo Android ARM64 em Kotlin + C++ (NDK), pensado para desenvolvimento diretamente no celular com um ambiente Android que suporte Gradle, SDK e NDK.

## O que este marco faz
- Abre uma tela Android em orientação horizontal.
- Inicializa um contexto OpenGL ES 2.0 e desenha um triângulo de diagnóstico.
- Permite selecionar um arquivo usando o seletor de documentos do Android.
- Lê metadados básicos e tenta reconhecer um descritor ISO9660.
- Não modifica a ISO e não executa o código do PS2.

## Requisitos
- Android SDK 35
- Android NDK instalado pelo ambiente de desenvolvimento
- CMake 3.22.1 ou compatível
- JDK compatível com Android Gradle Plugin 8.7.x
- ABI arm64-v8a

## Compilar no celular
Use um ambiente Android que consiga importar projetos Gradle/CMake, instalar SDK/NDK e executar tarefas Gradle. Nem todo editor Android oferece suporte a CMake/NDK; se o seu não oferecer, será necessário usar um ambiente de compilação Android compatível.

Abra/import the project root (a pasta que contém `settings.gradle.kts`), sincronize Gradle e compile o APK debug.

## Limitações e próximos marcos
Este é somente um esqueleto técnico. A ISO contém executáveis MIPS/Emotion Engine e recursos próprios do jogo; não contém o código-fonte original. Um port nativo real exige análise de formatos, reimplementação dos sistemas do jogo e testes extensivos. Não inclui BIOS, executáveis extraídos ou outros arquivos protegidos.
