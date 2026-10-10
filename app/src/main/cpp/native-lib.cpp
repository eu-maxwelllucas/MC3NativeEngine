
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
static constexpr uint16_t ELF_MACHINE_MIPS = 8;
static constexpr uint32_t ELF_PT_LOAD = 1;
static constexpr uint16_t ELF_ET_EXEC = 2;
static constexpr size_t ELF32_HEADER_SIZE = 52;
static constexpr size_t ELF32_PROGRAM_HEADER_SIZE = 32;
static constexpr uint16_t MAX_PROGRAM_HEADERS = 4096;

// ------------------------------------------------------------
// Leitura segura de arquivos
// ------------------------------------------------------------

static bool read_at(int fd, uint64_t offset,
                    void* buffer, size_t length) {
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

static bool range_fits(uint64_t offset,
                       uint64_t length,
                       uint64_t total) {
    return offset <= total && length <= total - offset;
}

// ------------------------------------------------------------
// Conversão de números
// ------------------------------------------------------------

static uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
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

static std::string hex_value(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

// ------------------------------------------------------------
// Manipulação de nomes
// ------------------------------------------------------------

static std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))
        );
    }

    return s;
}

static std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) return "";

    const auto last = s.find_last_not_of(" \t\r\n");

    return s.substr(0, last - first + 1);
}

static std::string clean_name(std::string s) {
    s = upper(s);

    const size_t semicolon = s.find(';');

    if (semicolon != std::string::npos) {
        s.resize(semicolon);
    }

    return s;
}

// ------------------------------------------------------------
// ISO9660
// ------------------------------------------------------------

struct IsoEntry {
    uint32_t extent = 0;
    uint32_t size = 0;
    bool directory = false;
    std::string name;
};

static bool parse_record(const uint8_t* p,
                         size_t available,
                         IsoEntry& entry) {
    if (available < 34 || p[0] < 34 || p[0] > available) {
        return false;
    }

    const uint8_t record_len = p[0];
    const uint8_t name_len = p[32];

    if (33u + name_len > record_len) {
        return false;
    }

    const uint32_t extent_le = le32(p + 2);
    const uint32_t extent_be = be32(p + 6);
    const uint32_t size_le = le32(p + 10);
    const uint32_t size_be = be32(p + 14);

    if (extent_le != extent_be || size_le != size_be) {
        return false;
    }

    entry.extent = extent_le;
    entry.size = size_le;
    entry.directory = (p[25] & 0x02) != 0;

    if (name_len == 1 && (p[33] == 0 || p[33] == 1)) {
        entry.name.clear();
    } else {
        entry.name.assign(
            reinterpret_cast<const char*>(p + 33),
            name_len
        );
    }

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

        if (offset >= image_size || dir.size == 0) continue;

        uint64_t safe_size =
            std::min<uint64_t>(dir.size, MAX_DIR_SIZE);

        if (safe_size > image_size - offset) {
            safe_size = image_size - offset;
        }

        std::vector<uint8_t> data(
            static_cast<size_t>(safe_size)
        );

        if (!read_at(fd, offset, data.data(), data.size())) {
            continue;
        }

        size_t pos = 0;

        while (pos < data.size()) {
            const uint8_t record_len = data[pos];

            if (record_len == 0) {
                pos = ((pos / SECTOR) + 1) * SECTOR;
                continue;
            }

            if (record_len > data.size() - pos) break;

            IsoEntry entry{};

            const bool valid = parse_record(
                data.data() + pos,
                data.size() - pos,
                entry
            );

            pos += record_len;

            if (!valid || entry.name.empty()) continue;

            const std::string normalized = clean_name(entry.name);

            const std::string path = dir.path.empty()
                ? normalized
                : dir.path + "/" + normalized;

            const uint64_t entry_offset =
                static_cast<uint64_t>(entry.extent) * SECTOR;

            if (entry_offset >= image_size) continue;

            if (!entry.directory && normalized == wanted) {
                if (!range_fits(
                        entry_offset, entry.size, image_size)) {
                    return false;
                }

                file_offset = entry_offset;
                file_size = entry.size;
                file_path = path;

                return true;
            }

            if (entry.directory &&
                entry.size > 0 &&
                entry.size <= MAX_DIR_SIZE &&
                range_fits(entry_offset, entry.size, image_size)) {
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

// ------------------------------------------------------------
// Leitura do executável indicado em SYSTEM.CNF
// ------------------------------------------------------------

static std::string get_boot_target(const std::string& content) {
    std::istringstream input(content);
    std::string line;

    while (std::getline(input, line)) {
        const std::string normalized = upper(line);

        if (normalized.find("BOOT") == std::string::npos) {
            continue;
        }

        const size_t equals = line.find('=');

        if (equals == std::string::npos) continue;

        std::string target = trim(line.substr(equals + 1));

        std::replace(
            target.begin(), target.end(), '\\', '/'
        );

        const size_t slash = target.find_last_of('/');

        if (slash != std::string::npos) {
            target = target.substr(slash + 1);
        }

        const size_t semicolon = target.find(';');

        if (semicolon != std::string::npos) {
            target.resize(semicolon);
        }

        return clean_name(trim(target));
    }

    return "";
}

// ------------------------------------------------------------
// Identificação da arquitetura ELF
// ------------------------------------------------------------

static std::string elf_machine_name(uint16_t machine) {
    switch (machine) {
        case 3:
            return "Intel 80386";
        case 8:
            return "MIPS";
        case 40:
            return "ARM";
        case 62:
            return "AMD64 / x86-64";
        case 183:
            return "AArch64";
        default:
            return "desconhecida";
    }
}

static std::string elf_type_name(uint16_t type) {
    switch (type) {
        case 0:
            return "ET_NONE (sem tipo definido)";
        case 1:
            return "ET_REL (arquivo relocável)";
        case 2:
            return "ET_EXEC (executável)";
        case 3:
            return "ET_DYN (objeto compartilhado)";
        default:
            return "tipo desconhecido";
    }
}

static std::string segment_flags(uint32_t flags) {
    std::string result;

    result += (flags & 4) ? 'R' : '-';
    result += (flags & 2) ? 'W' : '-';
    result += (flags & 1) ? 'X' : '-';

    return result;
}

// ------------------------------------------------------------
// Análise do cabeçalho e dos segmentos ELF32 little-endian
// ------------------------------------------------------------

static void analyze_elf(
    int fd,
    uint64_t exe_offset,
    uint32_t exe_size,
    std::ostringstream& out
) {
    out << "\n========== ANALISE ELF ==========\n";

    if (exe_size < 4) {
        out << "Erro: arquivo pequeno demais para um cabeçalho ELF.\n";
        return;
    }

    uint8_t header[64]{};

    const size_t bytes_to_read =
        std::min<size_t>(sizeof(header), exe_size);

    if (!read_at(fd, exe_offset, header, bytes_to_read)) {
        out << "Erro: falha ao ler o cabeçalho ELF.\n";
        return;
    }

    const bool elf_signature =
        header[0] == 0x7F &&
        header[1] == 'E' &&
        header[2] == 'L' &&
        header[3] == 'F';

    if (!elf_signature) {
        out << "Assinatura ELF: invalida\n";
        out << "Nao e possivel continuar a analise ELF.\n";
        return;
    }

    out << "Assinatura ELF: valida\n";

    const uint8_t elf_class = header[4];
    const uint8_t elf_data = header[5];
    const uint8_t ident_version = header[6];

    out << "Classe: ";

    if (elf_class == 1) {
        out << "ELF32 (32 bits)\n";
    } else if (elf_class == 2) {
        out << "ELF64 (64 bits)\n";
    } else {
        out << "desconhecida (" << static_cast<unsigned>(elf_class)
            << ")\n";
    }

    out << "Codificacao: ";

    if (elf_data == 1) {
        out << "little-endian\n";
    } else if (elf_data == 2) {
        out << "big-endian\n";
    } else {
        out << "desconhecida\n";
    }

    out << "Versao ELF identificadora: "
        << static_cast<unsigned>(ident_version) << "\n";

    if (elf_class != 1 || elf_data != 1) {
        out << "\nAnalise detalhada interrompida.\n";
        out << "Esta versao do analisador trata ELF32 little-endian.\n";
        return;
    }

    if (exe_size < ELF32_HEADER_SIZE ||
        bytes_to_read < ELF32_HEADER_SIZE) {
        out << "Erro: cabecalho ELF32 incompleto.\n";
        return;
    }

    const uint16_t e_type = le16(header + 16);
    const uint16_t e_machine = le16(header + 18);
    const uint32_t e_version = le32(header + 20);
    const uint32_t e_entry = le32(header + 24);
    const uint32_t e_phoff = le32(header + 28);
    const uint32_t e_shoff = le32(header + 32);
    const uint32_t e_flags = le32(header + 36);
    const uint16_t e_ehsize = le16(header + 40);
    const uint16_t e_phentsize = le16(header + 42);
    const uint16_t e_phnum = le16(header + 44);
    const uint16_t e_shentsize = le16(header + 46);
    const uint16_t e_shnum = le16(header + 48);
    const uint16_t e_shstrndx = le16(header + 50);

    out << "\n--- Cabecalho ELF ---\n";

    out << "Tipo: " << elf_type_name(e_type)
        << " (" << e_type << ")\n";

    out << "Maquina: " << elf_machine_name(e_machine)
        << " (" << e_machine << ")\n";

    out << "Versao do cabecalho: " << e_version << "\n";

    out << "Ponto de entrada: " << hex_value(e_entry) << "\n";

    out << "Offset da tabela de segmentos: "
        << e_phoff << " bytes\n";

    out << "Offset da tabela de secoes: "
        << e_shoff << " bytes\n";

    out << "Flags: " << hex_value(e_flags) << "\n";

    out << "Tamanho do cabecalho: "
        << e_ehsize << " bytes\n";

    out << "Tamanho de cada entrada de segmento: "
        << e_phentsize << " bytes\n";

    out << "Quantidade de segmentos declarados: "
        << e_phnum << "\n";

    out << "Tamanho de entrada de secao: "
        << e_shentsize << " bytes\n";

    out << "Quantidade de secoes declaradas: "
        << e_shnum << "\n";

    out << "Indice da secao de nomes: "
        << e_shstrndx << "\n";

    out << "\n--- Verificacoes ---\n";

    bool header_ok = true;

    if (ident_version != 1) {
        out << "AVISO: versao ELF identificadora inesperada.\n";
        header_ok = false;
    }

    if (e_version != 1) {
        out << "AVISO: versao ELF inesperada.\n";
        header_ok = false;
    }

    if (e_ehsize < ELF32_HEADER_SIZE ||
        e_ehsize > exe_size) {
        out << "ERRO: tamanho do cabecalho inconsistente.\n";
        header_ok = false;
    } else {
        out << "Tamanho do cabecalho: OK\n";
    }

    if (e_machine == ELF_MACHINE_MIPS) {
        out << "Arquitetura MIPS: identificada.\n";
    } else {
        out << "AVISO: arquitetura diferente de MIPS.\n";
    }

    if (e_type == ELF_ET_EXEC) {
        out << "Tipo executavel: identificado.\n";
    } else {
        out << "AVISO: tipo ELF nao e ET_EXEC.\n";
    }

    if (e_phnum == 0) {
        out << "AVISO: nenhuma entrada de segmento declarada.\n";
    }

    if (e_phnum > MAX_PROGRAM_HEADERS) {
        out << "ERRO: quantidade de segmentos acima do limite "
               "de seguranca.\n";
        out << "Tabela de segmentos nao sera analisada.\n";
        return;
    }

    if (e_phnum > 0 &&
        e_phentsize < ELF32_PROGRAM_HEADER_SIZE) {
        out << "ERRO: entrada de segmento menor que 32 bytes.\n";
        return;
    }

    const uint64_t table_size =
        static_cast<uint64_t>(e_phentsize) * e_phnum;

    if (e_phnum > 0 &&
        !range_fits(e_phoff, table_size, exe_size)) {
        out << "ERRO: tabela de segmentos ultrapassa o executavel.\n";
        out << "Analise dos segmentos interrompida.\n";
        return;
    }

    if (e_phnum == 0) {
        out << "Tabela de segmentos: nao ha entradas para analisar.\n";
        return;
    }

    out << "Tabela de segmentos: dentro dos limites do arquivo.\n";

    // O ponto de entrada costuma precisar estar em um segmento
    // carregavel para um executavel ELF convencional.
    bool entry_in_load_segment = false;
    bool found_load_segment = false;
    bool all_load_segments_valid = true;
    uint32_t load_count = 0;

    out << "\n--- Segmentos do programa ---\n";

    for (uint16_t i = 0; i < e_phnum; ++i) {
        const uint64_t ph_offset =
            static_cast<uint64_t>(e_phoff) +
            static_cast<uint64_t>(i) * e_phentsize;

        uint8_t ph[ELF32_PROGRAM_HEADER_SIZE]{};

        if (!read_at(
                fd,
                exe_offset + ph_offset,
                ph,
                sizeof(ph))) {
            out << "ERRO: falha ao ler segmento "
                << i << ".\n";
            all_load_segments_valid = false;
            break;
        }

        const uint32_t p_type = le32(ph + 0);
        const uint32_t p_offset = le32(ph + 4);
        const uint32_t p_vaddr = le32(ph + 8);
        const uint32_t p_paddr = le32(ph + 12);
        const uint32_t p_filesz = le32(ph + 16);
        const uint32_t p_memsz = le32(ph + 20);
        const uint32_t p_flags = le32(ph + 24);
        const uint32_t p_align = le32(ph + 28);

        out << "\nSegmento " << i << ":\n";
        out << "  Tipo: " << p_type;

        if (p_type == ELF_PT_LOAD) {
            out << " (PT_LOAD - carregavel)\n";
        } else {
            out << " (outro)\n";
        }

        out << "  Offset no executavel: "
            << hex_value(p_offset) << "\n";

        out << "  Endereco virtual: "
            << hex_value(p_vaddr) << "\n";

        out << "  Endereco fisico: "
            << hex_value(p_paddr) << "\n";

        out << "  Tamanho no arquivo: "
            << p_filesz << " bytes\n";

        out << "  Tamanho em memoria: "
            << p_memsz << " bytes\n";

        out << "  Permissoes: "
            << segment_flags(p_flags) << "\n";

        out << "  Alinhamento: "
            << p_align << "\n";

        if (p_type != ELF_PT_LOAD) continue;

        ++load_count;
        found_load_segment = true;

        bool segment_ok = true;

        if (!range_fits(p_offset, p_filesz, exe_size)) {
            out << "  ERRO: dados do segmento fora do arquivo.\n";
            segment_ok = false;
        }

        if (p_memsz < p_filesz) {
            out << "  ERRO: tamanho em memoria menor que "
                   "o tamanho no arquivo.\n";
            segment_ok = false;
        }

        if (p_align > 1 &&
            (p_align & (p_align - 1)) != 0) {
            out << "  AVISO: alinhamento nao e potencia de 2.\n";
        }

        if (segment_ok) {
            out << "  Validacao de limites: OK\n";
        } else {
            all_load_segments_valid = false;
        }

        const uint64_t segment_start = p_vaddr;
        const uint64_t segment_end =
            segment_start + static_cast<uint64_t>(p_memsz);

        if (p_memsz > 0 &&
            static_cast<uint64_t>(e_entry) >= segment_start &&
            static_cast<uint64_t>(e_entry) < segment_end &&
            (p_flags & 1) != 0) {
            entry_in_load_segment = true;
        }
    }

    out << "\n--- Resultado da analise de segmentos ---\n";

    out << "Segmentos carregaveis encontrados: "
        << load_count << "\n";

    if (!found_load_segment) {
        out << "AVISO: nenhum segmento PT_LOAD encontrado.\n";
    } else if (all_load_segments_valid) {
        out << "Limites dos segmentos carregaveis: OK\n";
    } else {
        out << "AVISO: foram encontradas inconsistencias "
               "nos segmentos carregaveis.\n";
    }

    if (entry_in_load_segment) {
        out << "Ponto de entrada: localizado em segmento "
               "carregavel com permissao de execucao.\n";
    } else {
        out << "AVISO: ponto de entrada nao foi confirmado "
               "em segmento carregavel executavel.\n";
    }

    if (header_ok && found_load_segment &&
        all_load_segments_valid) {
        out << "\nResultado estrutural: cabecalho e limites "
               "principais parecem consistentes.\n";
    } else {
        out << "\nResultado estrutural: existem avisos ou "
               "itens que precisam de investigacao.\n";
    }

    out << "\nNota: esta verificacao nao prova que o jogo "
           "possa ser executado.\n";
}

static void analyze_asset_header(
    int fd,
    uint64_t image_size,
    uint32_t root_extent,
    uint32_t root_size,
    std::ostringstream& out
) {
    out << "\n========== ANALISE ASSETS.DAT ==========\n";

    uint64_t file_offset = 0;
    uint32_t file_size = 0;
    std::string file_path;

    if (!find_file(
            fd, image_size,
            root_extent, root_size,
            "ASSETS.DAT",
            file_offset, file_size, file_path)) {
        out << "ASSETS.DAT nao encontrado ou intervalo invalido.\n";
        return;
    }

    out << "Arquivo: " << file_path << "\n";
    out << "Tamanho: " << file_size << " bytes\n";
    out << "Offset na ISO: " << file_offset << " bytes\n";

    if (file_size == 0 ||
        !range_fits(file_offset, file_size, image_size)) {
        out << "Erro: arquivo fora dos limites da ISO.\n";
        return;
    }

    // Le somente os primeiros 128 bytes.
    constexpr size_t HEADER_BYTES = 128;
    const size_t count = std::min<size_t>(
        file_size, HEADER_BYTES);

    std::vector<uint8_t> header(count);

    if (!read_at(fd, file_offset, header.data(), count)) {
        out << "Falha ao ler o cabecalho.\n";
        return;
    }

    out << "Bytes lidos: " << count << "\n";

    out << "\n--- Inicio em ASCII ---\n";
    for (size_t i = 0; i < std::min<size_t>(16, count); ++i) {
        unsigned char c = header[i];
        out << ((c >= 32 && c <= 126)
                    ? static_cast<char>(c) : '.');
    }
    out << "\n";

    out << "\n--- Assinatura em hexadecimal ---\n";
    for (size_t i = 0; i < std::min<size_t>(16, count); ++i) {
        if (i) out << ' ';
        out << std::hex << std::uppercase
            << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(header[i]);
    }
    out << std::dec << std::nouppercase
        << std::setfill(' ') << "\n";

    out << "\n--- Dump hexadecimal (128 bytes iniciais) ---\n";

    for (size_t row = 0; row < count; row += 16) {
        out << std::hex << std::uppercase
            << std::setw(4) << std::setfill('0')
            << row << "  ";

        const size_t end = std::min(row + 16, count);

        for (size_t i = row; i < end; ++i) {
            out << std::setw(2)
                << static_cast<unsigned>(header[i]) << ' ';
        }

        out << " |";

        for (size_t i = row; i < end; ++i) {
            unsigned char c = header[i];
            out << ((c >= 32 && c <= 126)
                        ? static_cast<char>(c) : '.');
        }

        out << "|\n";
    }

    out << std::dec << std::nouppercase
        << std::setfill(' ');

    out << "\n--- Palavras de 32 bits little-endian ---\n";

    for (size_t i = 0; i + 4 <= count && i < 32; i += 4) {
        uint32_t value =
            static_cast<uint32_t>(header[i]) |
            (static_cast<uint32_t>(header[i + 1]) << 8) |
            (static_cast<uint32_t>(header[i + 2]) << 16) |
            (static_cast<uint32_t>(header[i + 3]) << 24);

        out << "Offset +" << i << ": 0x"
            << std::hex << std::uppercase
            << std::setw(8) << std::setfill('0')
            << value << std::dec
            << std::nouppercase << std::setfill(' ')
            << " (" << value << ")\n";
    }

    out << "\nInspecao inicial concluida. "
           "O formato interno ainda precisa ser identificado.\n";
}

static void analyze_asset_structure(
    int fd,
    uint64_t image_size,
    uint32_t root_extent,
    uint32_t root_size,
    std::ostringstream& out
) {
    out << "\n========== AMOSTRAGEM DA ESTRUTURA ASSETS.DAT ==========\n";

    uint64_t file_offset = 0;
    uint32_t file_size = 0;
    std::string file_path;

    if (!find_file(
            fd, image_size,
            root_extent, root_size,
            "ASSETS.DAT",
            file_offset, file_size, file_path)) {
        out << "ASSETS.DAT nao encontrado.\n";
        return;
    }

    if (file_size < 4 ||
        !range_fits(file_offset, file_size, image_size)) {
        out << "Intervalo invalido; analise cancelada.\n";
        return;
    }

    const uint64_t size = file_size;
    const uint64_t sample_size = std::min<uint64_t>(256, size);

    std::vector<uint64_t> positions = {
        0,
        4096,
        size / 4,
        size / 2,
        (size * 3) / 4,
        size - sample_size
    };

    std::sort(positions.begin(), positions.end());
    positions.erase(
        std::unique(positions.begin(), positions.end()),
        positions.end()
    );

    out << "Arquivo: " << file_path << "\n";
    out << "Tamanho: " << size << " bytes\n";
    out << "Amostras: " << positions.size()
        << " (ate 256 bytes cada)\n";

    for (size_t n = 0; n < positions.size(); ++n) {
        const uint64_t rel = positions[n];
        const size_t amount = static_cast<size_t>(
            std::min<uint64_t>(sample_size, size - rel)
        );

        std::vector<uint8_t> bytes(amount);

        if (!read_at(
                fd, file_offset + rel,
                bytes.data(), amount)) {
            out << "\nErro ao ler amostra " << n << ".\n";
            continue;
        }

        size_t printable = 0;
        for (uint8_t b : bytes) {
            if (b >= 32 && b <= 126) ++printable;
        }

        out << "\n--- Amostra " << n
            << " | offset relativo 0x"
            << std::hex << rel << std::dec
            << " (" << rel << " bytes) ---\n";

        out << "Inicio HEX: ";
        const size_t count = std::min<size_t>(32, bytes.size());

        for (size_t i = 0; i < count; ++i) {
            if (i) out << ' ';
            out << std::hex << std::uppercase
                << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(bytes[i]);
        }

        out << std::dec << std::nouppercase
            << std::setfill(' ') << "\n";

        out << "ASCII imprimivel: "
            << printable << "/" << bytes.size() << "\n";

        out << "Possiveis offsets de 32 bits: ";

        size_t found = 0;
        for (size_t i = 0;
             i + 4 <= bytes.size() && found < 8;
             i += 4) {
            const uint32_t value = le32(bytes.data() + i);

            if (value >= 16 &&
                static_cast<uint64_t>(value) < size) {
                if (found) out << "; ";

                out << "+0x" << std::hex << i
                    << "=0x" << value
                    << std::dec << " (" << value << ")";

                ++found;
            }
        }

        if (!found) out << "nenhum encontrado";
        out << "\n";
    }

    out << "\nAviso: offsets candidatos ainda precisam "
           "ser validados; isso nao confirma modelos.\n";
}


struct AssetRecord {
    std::string path;
    uint32_t size;
    uint32_t extent;
};

static std::string asset_extension(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    const size_t dot = name.find_last_of('.');

    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash)) {
        return "";
    }

    return upper(name.substr(dot));
}

static bool is_asset_candidate(const AssetRecord& file) {
    const std::string ext = asset_extension(file.path);
    const std::string name = upper(file.path);

    static const std::set<std::string> extensions = {
        ".BIN", ".DAT", ".PAK", ".VIV", ".ARC", ".IMG",
        ".GEO", ".MDL", ".MESH", ".MODEL", ".CAR", ".VEH",
        ".TEX", ".TXD", ".PVR", ".TM2", ".VIF", ".VPK",
        ".WAD", ".PS2"
    };

    return extensions.count(ext) > 0 ||
           name.find("CAR") != std::string::npos ||
           name.find("VEH") != std::string::npos ||
           name.find("MODEL") != std::string::npos ||
           name.find("MESH") != std::string::npos ||
           name.find("ECLIPSE") != std::string::npos;
}

static void scan_iso_assets(
    int fd,
    uint64_t image_size,
    uint32_t root_extent,
    uint32_t root_size,
    std::ostringstream& out
) {
    constexpr size_t MAX_FILES = 30000;
    constexpr size_t MAX_LIST = 150;

    std::queue<DirTask> pending;
    std::set<std::pair<uint32_t, uint32_t>> visited;
    std::vector<AssetRecord> files;

    pending.push({root_extent, root_size, "", 0});

    size_t directories_read = 0;
    bool truncated = false;

    while (!pending.empty() &&
           visited.size() < MAX_DIRS &&
           files.size() < MAX_FILES) {

        DirTask dir = pending.front();
        pending.pop();

        if (dir.depth > 32) continue;

        if (!visited.insert(
                {dir.extent, dir.size}).second) {
            continue;
        }

        const uint64_t offset =
            static_cast<uint64_t>(dir.extent) * SECTOR;

        if (offset >= image_size || dir.size == 0) continue;

        uint64_t safe_size =
            std::min<uint64_t>(dir.size, MAX_DIR_SIZE);

        safe_size = std::min<uint64_t>(
            safe_size, image_size - offset);

        if (safe_size == 0) continue;

        std::vector<uint8_t> data(
            static_cast<size_t>(safe_size));

        if (!read_at(fd, offset, data.data(), data.size())) {
            continue;
        }

        ++directories_read;

        size_t pos = 0;

        while (pos < data.size()) {
            const uint8_t record_len = data[pos];

            if (record_len == 0) {
                pos = ((pos / SECTOR) + 1) * SECTOR;
                continue;
            }

            if (record_len > data.size() - pos) break;

            IsoEntry entry{};

            const bool valid = parse_record(
                data.data() + pos,
                data.size() - pos,
                entry);

            pos += record_len;

            if (!valid || entry.name.empty()) continue;

            const std::string name = clean_name(entry.name);

            const std::string path = dir.path.empty()
                ? name
                : dir.path + "/" + name;

            const uint64_t file_offset =
                static_cast<uint64_t>(entry.extent) * SECTOR;

            if (file_offset >= image_size) continue;

            if (entry.directory) {
                if (entry.size > 0 &&
                    entry.size <= MAX_DIR_SIZE &&
                    range_fits(
                        file_offset, entry.size, image_size)) {
                    pending.push({
                        entry.extent,
                        entry.size,
                        path,
                        dir.depth + 1
                    });
                }
            } else {
                files.push_back({
                    path, entry.size, entry.extent
                });

                if (files.size() >= MAX_FILES) {
                    truncated = true;
                    break;
                }
            }
        }
    }

    if (!pending.empty() || visited.size() >= MAX_DIRS) {
        truncated = true;
    }

    std::vector<AssetRecord> candidates;

    for (const auto& file : files) {
        if (is_asset_candidate(file)) {
            candidates.push_back(file);
        }
    }

    auto largest_first =
        [](const AssetRecord& a, const AssetRecord& b) {
            return a.size > b.size;
        };

    std::sort(
        candidates.begin(), candidates.end(), largest_first);

    std::sort(
        files.begin(), files.end(), largest_first);

    out << "\n========== SCANNER DE ASSETS ==========\n";
    out << "Diretorios lidos: " << directories_read << "\n";
    out << "Arquivos encontrados: " << files.size() << "\n";
    out << "Candidatos a assets: " << candidates.size() << "\n";

    if (truncated) {
        out << "AVISO: limite de seguranca atingido; "
               "a varredura pode estar incompleta.\n";
    }

    out << "\n--- Candidatos a modelos/texturas/pacotes ---\n";

    const size_t count =
        std::min(candidates.size(), MAX_LIST);

    for (size_t i = 0; i < count; ++i) {
        const auto& f = candidates[i];

        out << "[" << i + 1 << "] "
            << f.path
            << " | " << f.size << " bytes"
            << " | setor " << f.extent << "\n";
    }

    if (candidates.empty()) {
        out << "Nenhum candidato identificado por nome "
               "ou extensao.\n";
    }

    out << "\n--- Maiores arquivos encontrados ---\n";

    const size_t largest =
        std::min(files.size(), static_cast<size_t>(30));

    for (size_t i = 0; i < largest; ++i) {
        const auto& f = files[i];

        out << "[" << i + 1 << "] "
            << f.path
            << " | " << f.size << " bytes"
            << " | setor " << f.extent << "\n";
    }

    out << "\nO scanner lista candidatos; nao decodifica "
           "formatos proprietarios nem confirma o modelo "
           "do Eclipse.\n";
}

// ------------------------------------------------------------
// Funcao chamada pelo Kotlin via JNI
// ------------------------------------------------------------

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_mc3native_MainActivity_inspectIso(
    JNIEnv* env,
    jobject,
    jint fd
) {
    std::ostringstream out;

    struct stat st{};

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        return env->NewStringUTF(
            "Arquivo invalido ou tamanho nao disponivel."
        );
    }

    const uint64_t image_size =
        static_cast<uint64_t>(st.st_size);

    out << "MC3 Native Engine - Diagnostico 0.5\n\n";

    out << "Tamanho: " << std::fixed << std::setprecision(2)
        << (image_size / (1024.0 * 1024.0))
        << " MiB\n";

    // Descritor primario de volume ISO9660.
    uint8_t pvd[SECTOR]{};

    if (image_size < 17 * SECTOR ||
        !read_at(fd, 16 * SECTOR, pvd, sizeof(pvd)) ||
        pvd[0] != 1 ||
        std::memcmp(pvd + 1, "CD001", 5) != 0 ||
        pvd[6] != 1) {
        out << "Descritor ISO9660: nao identificado\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Descritor ISO9660: identificado\n";

    IsoEntry root{};

    if (!parse_record(
            pvd + 156,
            sizeof(pvd) - 156,
            root) ||
        !root.directory) {
        out << "Erro: diretorio raiz invalido.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    const uint64_t root_offset =
        static_cast<uint64_t>(root.extent) * SECTOR;

    if (!range_fits(root_offset, root.size, image_size)) {
        out << "Erro: diretorio raiz fora dos limites da ISO.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Diretorio raiz: identificado\n";

    // Procura SYSTEM.CNF.
    uint64_t cnf_offset = 0;
    uint32_t cnf_size = 0;
    std::string cnf_path;

    if (!find_file(
            fd, image_size,
            root.extent, root.size,
            "SYSTEM.CNF",
            cnf_offset, cnf_size, cnf_path)) {
        out << "\nSYSTEM.CNF: nao encontrado\n";
        out << "Nao foi possivel continuar a analise.\n";
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

    if (content.empty() || content.back() != '\n') {
        out << '\n';
    }

    out << "--- Fim ---\n";

    // Identifica o executavel declarado pelo disco.
    const std::string target = get_boot_target(content);

    if (target.empty()) {
        out << "\nExecutavel: nao foi possivel extrair "
               "a referencia BOOT/BOOT2.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "\nExecutavel esperado: " << target << "\n";

    uint64_t exe_offset = 0;
    uint32_t exe_size = 0;
    std::string exe_path;

    if (!find_file(
            fd, image_size,
            root.extent, root.size,
            target,
            exe_offset, exe_size, exe_path)) {
        out << "Executavel: nao encontrado nos diretorios "
               "ISO9660.\n";
        out << "Isso nao prova que a ISO esteja corrompida.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Executavel: encontrado\n";
    out << "Caminho: " << exe_path << "\n";
    out << "Tamanho: " << exe_size << " bytes\n";
    out << "Offset na ISO: " << exe_offset << " bytes\n";

    // Confirma que todo o executavel esta dentro da imagem.
    if (!range_fits(exe_offset, exe_size, image_size)) {
        out << "Erro: executavel ultrapassa os limites da ISO.\n";
        return env->NewStringUTF(out.str().c_str());
    }

    // Analisa o cabeçalho ELF e os segmentos do executavel.
    analyze_elf(fd, exe_offset, exe_size, out);
    scan_iso_assets(
    fd,
    image_size,
    root.extent,
    root.size,
    out
);
    
    analyze_asset_header(
        fd,
        image_size,
        root.extent,
        root.size,
        out
    );
  
    out << "\n========== LIMITACOES ==========\n";
    out << "- O executavel foi analisado, nao executado.\n";
    out << "- Ainda nao existe um motor que execute o jogo PS2.\n";
    out << "- A analise estrutural nao garante que o arquivo "
           "esteja completo ou funcional.\n";
    out << "- A compatibilidade real exige implementar ou "
           "integrar componentes de emulacao de PS2.\n";

    return env->NewStringUTF(out.str().c_str());
}
