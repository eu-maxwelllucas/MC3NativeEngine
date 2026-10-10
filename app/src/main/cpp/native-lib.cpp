
#include <jni.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <queue>
#include <set>
#include <iomanip>
#include <cctype>

static constexpr uint64_t SECTOR_SIZE = 2048;
static constexpr uint64_t MAX_DIR_SIZE = 16 * 1024 * 1024;
static constexpr uint64_t MAX_CNF_SIZE = 64 * 1024;
static constexpr size_t MAX_DIRS = 10000;

static bool read_at(int fd, uint64_t offset, void* buffer, size_t length) {
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;

    while (done < length) {
        ssize_t n = pread(fd, out + done, length - done,
                          static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

static uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static std::string upper(std::string s) {
    for (char& c : s)
        c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string clean_name(std::string s) {
    s = upper(s);
    size_t version = s.find(';');
    if (version != std::string::npos) s.resize(version);
    return s;
}

struct IsoEntry {
    uint32_t extent;
    uint32_t size;
    bool directory;
    std::string name;
};

static bool parse_record(const uint8_t* p, size_t available,
                         IsoEntry& entry) {
    if (available < 34 || p[0] < 34 || p[0] > available)
        return false;

    const uint8_t record_len = p[0];
    const uint8_t name_len = p[32];

    if (33u + name_len > record_len)
        return false;

    // ISO9660 stores both little-endian and big-endian values.
    const uint32_t extent_le = le32(p + 2);
    const uint32_t extent_be = be32(p + 6);
    const uint32_t size_le = le32(p + 10);
    const uint32_t size_be = be32(p + 14);

    if (extent_le != extent_be || size_le != size_be)
        return false;

    entry.extent = extent_le;
    entry.size = size_le;
    entry.directory = (p[25] & 0x02) != 0;

    // Identifier 0 means "." and 1 means "..".
    if (name_len == 1 && (p[33] == 0 || p[33] == 1))
        entry.name.clear();
    else
        entry.name.assign(
            reinterpret_cast<const char*>(p + 33), name_len);

    return true;
}

struct DirTask {
    uint32_t extent;
    uint32_t size;
    std::string path;
    unsigned depth;
};

static bool find_system_cnf(
    int fd,
    uint64_t image_size,
    uint32_t root_extent,
    uint32_t root_size,
    uint64_t& cnf_offset,
    uint32_t& cnf_size,
    std::string& cnf_path,
    std::string& error
) {
    std::queue<DirTask> pending;
    std::set<std::pair<uint32_t, uint32_t>> visited;

    pending.push({root_extent, root_size, "", 0});

    while (!pending.empty() && visited.size() < MAX_DIRS) {
        DirTask dir = pending.front();
        pending.pop();

        if (dir.depth > 32) continue;

        auto key = std::make_pair(dir.extent, dir.size);
        if (!visited.insert(key).second) continue;

        uint64_t offset =
            static_cast<uint64_t>(dir.extent) * SECTOR_SIZE;

        if (offset >= image_size || dir.size == 0) continue;

        uint64_t safe_size = std::min<uint64_t>(
            dir.size, MAX_DIR_SIZE);

        if (offset + safe_size > image_size)
            safe_size = image_size - offset;

        std::vector<uint8_t> data(
            static_cast<size_t>(safe_size));

        if (!read_at(fd, offset, data.data(), data.size()))
            continue;

        size_t pos = 0;
        while (pos < data.size()) {
            uint8_t record_len = data[pos];

            // A zero-length record marks padding until the next sector.
            if (record_len == 0) {
                pos = ((pos / SECTOR_SIZE) + 1) * SECTOR_SIZE;
                continue;
            }

            if (record_len > data.size() - pos)
                break;

            IsoEntry entry{};
            if (!parse_record(
                    data.data() + pos,
                    data.size() - pos,
                    entry)) {
                pos += record_len;
                continue;
            }

            pos += record_len;

            if (entry.name.empty()) continue;

            std::string name = clean_name(entry.name);
            std::string path = dir.path.empty()
                ? entry.name
                : dir.path + "/" + entry.name;

            uint64_t file_offset =
                static_cast<uint64_t>(entry.extent) * SECTOR_SIZE;

            if (file_offset >= image_size) continue;

            if (name == "SYSTEM.CNF" && !entry.directory) {
                cnf_offset = file_offset;
                cnf_size = static_cast<uint32_t>(
                    std::min<uint64_t>(
                        entry.size, MAX_CNF_SIZE));

                if (file_offset + cnf_size > image_size)
                    cnf_size = static_cast<uint32_t>(
                        image_size - file_offset);

                cnf_path = path;
                return true;
            }

            if (entry.directory &&
                entry.size > 0 &&
                entry.size <= MAX_DIR_SIZE &&
                file_offset + entry.size <= image_size) {
                pending.push({
                    entry.extent, entry.size, path, dir.depth + 1
                });
            }
        }
    }

    error = visited.size() >= MAX_DIRS
        ? "Limite de diretórios alcançado."
        : "SYSTEM.CNF não encontrado nos diretórios ISO9660.";

    return false;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_mc3native_MainActivity_inspectIso(
    JNIEnv* env, jobject, jint fd
) {
    std::ostringstream out;
    struct stat st{};

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        return env->NewStringUTF(
            "Arquivo inválido ou tamanho não disponível.");
    }

    const uint64_t image_size =
        static_cast<uint64_t>(st.st_size);

    out << "MC3 Native Engine - Diagnóstico 0.3\n\n";
    out << "Tamanho: " << std::fixed << std::setprecision(2)
        << (image_size / (1024.0 * 1024.0)) << " MiB\n";

    uint8_t pvd[SECTOR_SIZE]{};

    if (image_size < 17 * SECTOR_SIZE ||
        !read_at(fd, 16 * SECTOR_SIZE, pvd, sizeof(pvd)) ||
        pvd[0] != 1 ||
        std::memcmp(pvd + 1, "CD001", 5) != 0 ||
        pvd[6] != 1) {
        out << "Descritor ISO9660: não identificado\n";
        out << "\nNão foi possível localizar um descritor "
               "primário ISO9660 válido no setor esperado.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Descritor ISO9660: identificado\n";

    // The root directory record begins at byte 156 of the PVD.
    IsoEntry root{};
    if (!parse_record(pvd + 156, sizeof(pvd) - 156, root) ||
        !root.directory) {
        out << "Erro: diretório raiz inválido.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Diretório raiz: identificado\n";
    out << "\nProcurando SYSTEM.CNF na estrutura do disco...\n";

    uint64_t cnf_offset = 0;
    uint32_t cnf_size = 0;
    std::string cnf_path;
    std::string error;

    if (!find_system_cnf(
            fd, image_size,
            root.extent, root.size,
            cnf_offset, cnf_size, cnf_path, error)) {
        out << "SYSTEM.CNF: não localizado\n";
        out << "Detalhe: " << error << "\n";
        out << "\nEste resultado não prova que a ISO esteja "
               "corrompida; a imagem pode usar outra estrutura.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "SYSTEM.CNF: encontrado\n";
    out << "Caminho: " << cnf_path << "\n";
    out << "Tamanho: " << cnf_size << " bytes\n";

    std::vector<char> cnf(cnf_size + 1, '\0');

    if (cnf_size == 0 ||
        !read_at(fd, cnf_offset, cnf.data(), cnf_size)) {
        out << "Erro ao ler o conteúdo de SYSTEM.CNF.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    std::string content(cnf.data());
    out << "\n--- Conteúdo de SYSTEM.CNF ---\n";
    out << content << "\n";
    out << "--- Fim do arquivo ---\n";

    std::istringstream lines(content);
    std::string line;
    std::string boot_line;

    while (std::getline(lines, line)) {
        std::string normalized = upper(line);
        size_t pos = normalized.find("BOOT2");

        if (pos == std::string::npos)
            pos = normalized.find("BOOT");

        if (pos != std::string::npos && normalized.find('=') !=
                std::string::npos) {
            boot_line = line;
            break;
        }
    }

    if (!boot_line.empty()) {
        out << "\nReferência de inicialização: "
            << boot_line << "\n";
    } else {
        out << "\nAviso: nenhuma linha BOOT/BOOT2 reconhecida.\n";
    }

    out << "\nLimitações:\n";
    out << "- A ISO foi analisada, mas o jogo não foi executado.\n";
    out << "- Esta versão interpreta diretórios ISO9660.\n";
    out << "- Ainda não implementa o motor do jogo PS2.\n";

    return env->NewStringUTF(out.str().c_str());
}
