
#include <jni.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <queue>
#include <set>
#include <algorithm>
#include <cctype>
#include <iomanip>

static constexpr uint64_t SECTOR = 2048;
static constexpr uint64_t MAX_DIR_SIZE = 16 * 1024 * 1024;
static constexpr size_t MAX_DIRS = 10000;
static constexpr uint32_t MAX_CNF_SIZE = 64 * 1024;

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
    for (char& c : s) {
        c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

static std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";

    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string clean_name(std::string s) {
    s = upper(s);
    const size_t semicolon = s.find(';');
    if (semicolon != std::string::npos)
        s.resize(semicolon);
    return s;
}

struct IsoEntry {
    uint32_t extent = 0;
    uint32_t size = 0;
    bool directory = false;
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

    const uint32_t extent_le = le32(p + 2);
    const uint32_t extent_be = be32(p + 6);
    const uint32_t size_le = le32(p + 10);
    const uint32_t size_be = be32(p + 14);

    if (extent_le != extent_be || size_le != size_be)
        return false;

    entry.extent = extent_le;
    entry.size = size_le;
    entry.directory = (p[25] & 0x02) != 0;

    // Os identificadores 0 e 1 representam "." e "..".
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

static bool find_file(
    int fd,
    uint64_t image_size,
    uint32_t root_extent,
    uint32_t root_size,
    const std::string& wanted_name,
    uint64_t& file_offset,
    uint32_t& file_size,
    std::string& file_path
) {
    std::queue<DirTask> pending;
    std::set<std::pair<uint32_t, uint32_t>> visited;

    pending.push({root_extent, root_size, "", 0});
    const std::string wanted = clean_name(wanted_name);

    while (!pending.empty() && visited.size() < MAX_DIRS) {
        DirTask dir = pending.front();
        pending.pop();

        if (dir.depth > 32) continue;

        const auto key = std::make_pair(dir.extent, dir.size);
        if (!visited.insert(key).second) continue;

        const uint64_t offset =
            static_cast<uint64_t>(dir.extent) * SECTOR;

        if (offset >= image_size || dir.size == 0)
            continue;

        uint64_t safe_size = std::min<uint64_t>(
            dir.size, MAX_DIR_SIZE);

        if (safe_size > image_size - offset)
            safe_size = image_size - offset;

        std::vector<uint8_t> data(
            static_cast<size_t>(safe_size));

        if (!read_at(fd, offset, data.data(), data.size()))
            continue;

        size_t pos = 0;

        while (pos < data.size()) {
            const uint8_t record_len = data[pos];

            if (record_len == 0) {
                pos = ((pos / SECTOR) + 1) * SECTOR;
                continue;
            }

            if (record_len > data.size() - pos)
                break;

            IsoEntry entry{};
            const bool valid = parse_record(
                data.data() + pos,
                data.size() - pos,
                entry);

            pos += record_len;
            if (!valid || entry.name.empty())
                continue;

            const std::string normalized =
                clean_name(entry.name);

            const std::string path = dir.path.empty()
                ? normalized
                : dir.path + "/" + normalized;

            const uint64_t entry_offset =
                static_cast<uint64_t>(entry.extent) * SECTOR;

            if (entry_offset >= image_size)
                continue;

            if (!entry.directory && normalized == wanted) {
                file_offset = entry_offset;
                file_size = entry.size;
                file_path = path;

                if (file_size > image_size - file_offset)
                    return false;

                return true;
            }

            if (entry.directory && entry.size > 0 &&
                entry.size <= MAX_DIR_SIZE &&
                entry_offset + entry.size <= image_size) {
                pending.push({
                    entry.extent,
                    entry.size,
                    path,
                    dir.depth + 1
                });
            }
        }
    }

    return false;
}

static std::string get_boot_target(const std::string& content) {
    std::istringstream input(content);
    std::string line;

    while (std::getline(input, line)) {
        const std::string normalized = upper(line);
        if (normalized.find("BOOT") == std::string::npos)
            continue;

        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string target = trim(line.substr(equals + 1));

        // Converte a referência do disco em um nome de arquivo.
        std::replace(target.begin(), target.end(), '\\', '/');

        const size_t slash = target.find_last_of('/');
        if (slash != std::string::npos)
            target = target.substr(slash + 1);

        const size_t semicolon = target.find(';');
        if (semicolon != std::string::npos)
            target.resize(semicolon);

        return clean_name(trim(target));
    }

    return "";
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

    out << "MC3 Native Engine - Diagnóstico 0.4\n\n";
    out << "Tamanho: " << std::fixed << std::setprecision(2)
        << (image_size / (1024.0 * 1024.0)) << " MiB\n";

    uint8_t pvd[SECTOR]{};

    if (image_size < 17 * SECTOR ||
        !read_at(fd, 16 * SECTOR, pvd, sizeof(pvd)) ||
        pvd[0] != 1 ||
        std::memcmp(pvd + 1, "CD001", 5) != 0 ||
        pvd[6] != 1) {
        out << "Descritor ISO9660: não identificado\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Descritor ISO9660: identificado\n";

    IsoEntry root{};
    if (!parse_record(pvd + 156, sizeof(pvd) - 156, root) ||
        !root.directory) {
        out << "Erro: diretório raiz inválido.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Diretório raiz: identificado\n";

    uint64_t cnf_offset = 0;
    uint32_t cnf_size = 0;
    std::string cnf_path;

    if (!find_file(fd, image_size, root.extent, root.size,
                   "SYSTEM.CNF", cnf_offset, cnf_size, cnf_path)) {
        out << "SYSTEM.CNF: não encontrado\n";
        out << "Não foi possível continuar a análise.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "\nSYSTEM.CNF: encontrado\n";
    out << "Caminho: " << cnf_path << "\n";
    out << "Tamanho: " << cnf_size << " bytes\n";

    if (cnf_size == 0 || cnf_size > MAX_CNF_SIZE) {
        out << "Tamanho inesperado para SYSTEM.CNF.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    std::vector<char> cnf(cnf_size + 1, '\0');

    if (!read_at(fd, cnf_offset, cnf.data(), cnf_size)) {
        out << "Falha ao ler SYSTEM.CNF.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    const std::string content(cnf.data(), cnf_size);
    out << "\n--- SYSTEM.CNF ---\n";
    out << content;
    if (content.empty() || content.back() != '\n')
        out << '\n';
    out << "--- Fim ---\n";

    const std::string target = get_boot_target(content);

    if (target.empty()) {
        out << "\nExecutável: não foi possível extrair "
               "a referência BOOT/BOOT2.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "\nExecutável esperado: " << target << "\n";

    uint64_t exe_offset = 0;
    uint32_t exe_size = 0;
    std::string exe_path;

    if (!find_file(fd, image_size, root.extent, root.size,
                   target, exe_offset, exe_size, exe_path)) {
        out << "Executável: não encontrado nos diretórios ISO9660.\n";
        out << "Isso não prova que a ISO esteja corrompida.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Executável: encontrado\n";
    out << "Caminho: " << exe_path << "\n";
    out << "Tamanho: " << exe_size << " bytes\n";
    out << "Offset na ISO: " << exe_offset << " bytes\n";

    if (exe_size < 4) {
        out << "Arquivo pequeno demais para analisar o cabeçalho.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    uint8_t header[64]{};
    const size_t bytes_to_read =
        std::min<size_t>(sizeof(header), exe_size);

    if (!read_at(fd, exe_offset, header, bytes_to_read)) {
        out << "Falha ao ler o cabeçalho do executável.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "\nPrimeiros bytes:";
    for (size_t i = 0; i < std::min<size_t>(16, bytes_to_read); ++i) {
        out << ' ' << std::hex << std::uppercase
            << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(header[i]);
    }
    out << std::dec << std::setfill(' ') << "\n";

    const bool elf = bytes_to_read >= 4 &&
        header[0] == 0x7F &&
        header[1] == 'E' &&
        header[2] == 'L' &&
        header[3] == 'F';

    if (elf) {
        out << "Cabeçalho: assinatura ELF identificada\n";
        if (bytes_to_read >= 6) {
            out << "Classe ELF: "
                << (header[4] == 1 ? "32 bits" :
                    header[4] == 2 ? "64 bits" : "desconhecida")
                << "\n";
            out << "Endian: "
                << (header[5] == 1 ? "little-endian" :
                    header[5] == 2 ? "big-endian" : "desconhecido")
                << "\n";
        }
    } else {
        out << "Cabeçalho: assinatura ELF não identificada.\n";
        out << "É necessário investigar o formato deste arquivo.\n";
    }

    out << "\nLimitações:\n";
    out << "- O executável foi localizado e inspecionado, não executado.\n";
    out << "- Ainda não existe um motor que execute o jogo PS2.\n";

    return env->NewStringUTF(out.str().c_str());
}
