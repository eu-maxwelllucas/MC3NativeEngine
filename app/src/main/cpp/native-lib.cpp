
#include <jni.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>

static bool read_at(int fd, uint64_t offset, void* buffer, size_t length) {
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;

    while (done < length) {
        ssize_t n = pread(
            fd,
            out + done,
            length - done,
            static_cast<off_t>(offset + done)
        );

        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }

    return true;
}

static bool contains_text(const std::vector<uint8_t>& data,
                          const std::string& target) {
    if (target.empty() || data.size() < target.size()) return false;

    return std::search(
        data.begin(), data.end(),
        target.begin(), target.end()
    ) != data.end();
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_mc3native_MainActivity_inspectIso(
    JNIEnv* env, jobject, jint fd
) {
    struct stat st{};
    std::ostringstream out;

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        return env->NewStringUTF(
            "Arquivo inválido ou tamanho não disponível."
        );
    }

    const uint64_t size = static_cast<uint64_t>(st.st_size);

    out << "MC3 Native Engine - Diagnóstico 0.2\n\n";
    out << "Tamanho: " << (size / (1024.0 * 1024.0))
        << " MiB\n";

    // Verificação preliminar do descritor ISO9660.
    uint8_t pvd[2048]{};
    bool iso9660 = false;

    if (size >= 17 * 2048 &&
        read_at(fd, 16 * 2048, pvd, sizeof(pvd))) {
        iso9660 = pvd[0] == 1 &&
            std::memcmp(pvd + 1, "CD001", 5) == 0 &&
            pvd[6] == 1;
    }

    out << "Descritor ISO9660: "
        << (iso9660 ? "identificado" : "não identificado")
        << "\n";

    // Procura assinaturas conhecidas em uma amostra inicial.
    // Isso não é uma varredura completa nem confirma a estrutura do disco.
    const size_t sample_size = static_cast<size_t>(
        std::min<uint64_t>(size, 8 * 1024 * 1024)
    );

    std::vector<uint8_t> sample(sample_size);
    bool sample_read = sample_size > 0 &&
        read_at(fd, 0, sample.data(), sample.size());

    if (!sample_read) {
        out << "Não foi possível ler a amostra inicial.\n";
    } else {
        const std::string upper_targets[] = {
            "SYSTEM.CNF", "SLUS_", "SLES_", "SCUS_", "SCES_"
        };

        out << "\nBusca preliminar nos primeiros 8 MiB:\n";

        for (const auto& target : upper_targets) {
            bool found = contains_text(sample, target);
            out << "- " << target << ": "
                << (found ? "encontrado" : "não encontrado")
                << "\n";
        }
    }

    out << "\nLimitações:\n";
    out << "- A busca examina apenas uma amostra inicial.\n";
    out << "- Não encontrar um nome não significa que ele não exista no disco.\n";
    out << "- Ainda não foi interpretada a estrutura completa da ISO.\n";
    out << "- Este protótipo não executa o jogo.\n";

    return env->NewStringUTF(out.str().c_str());
}
