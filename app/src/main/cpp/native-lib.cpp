#include <jni.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>

static bool read_at(int fd, uint64_t offset, void* buffer, size_t length) {
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < length) {
        const ssize_t n = pread(fd, out + done, length - done,
                                static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_mc3native_MainActivity_inspectIso(JNIEnv* env, jobject, jint fd) {
    struct stat st{};
    std::ostringstream out;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        out << "Arquivo inválido ou tamanho não disponível.";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Arquivo selecionado\nTamanho: " << (st.st_size / (1024.0 * 1024.0))
        << " MiB\n";

    // ISO9660 Primary Volume Descriptor: sector 16, byte offset 16*2048.
    uint8_t pvd[2048]{};
    if (st.st_size >= 17 * 2048 && read_at(fd, 16 * 2048, pvd, sizeof(pvd))) {
        const bool iso9660 = pvd[0] == 1 &&
            std::memcmp(pvd + 1, "CD001", 5) == 0 && pvd[6] == 1;
        out << "Descritor ISO9660 no setor 16: "
            << (iso9660 ? "encontrado" : "não identificado") << "\n";
    } else {
        out << "Arquivo pequeno demais para um descritor ISO9660 padrão.\n";
    }

    // PS2 disc images can use layouts that do not expose a conventional ISO9660 PVD.
    uint8_t header[64]{};
    if (read_at(fd, 0, header, sizeof(header))) {
        std::string first(reinterpret_cast<char*>(header), sizeof(header));
        if (first.find("PLAYSTATION") != std::string::npos ||
            first.find("PlayStation") != std::string::npos) {
            out << "Texto relacionado a PlayStation encontrado no início do arquivo.\n";
        } else {
            out << "Cabeçalho inicial lido; isso não confirma nem descarta uma ISO de PS2.\n";
        }
    }

    out << "\nDiagnóstico inicial concluído.\n"
        << "Próximo passo: analisar a estrutura do disco e os formatos dos recursos.\n"
        << "Este protótipo ainda não executa o jogo.";
    return env->NewStringUTF(out.str().c_str());
}
