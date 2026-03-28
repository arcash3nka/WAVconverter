#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <ncurses.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────

static constexpr size_t   kBufferSize     = 4096;
static constexpr size_t   kMaxExtLength   = 32;
static constexpr size_t   kMaxNameLength  = 64;
static constexpr uint32_t kFmtChunkSize   = 16;
static constexpr uint16_t kPcmAudioFormat = 1;

// ─────────────────────────────────────────────────────────────
// WAV chunk structs  (packed — matches on-disk layout exactly)
// ─────────────────────────────────────────────────────────────

#pragma pack(push, 1)

struct RiffChunk {
    char     id[4];       // "RIFF"
    uint32_t size;
    char     format[4];   // "WAVE"
};

struct FmtChunk {
    char     id[4];       // "fmt "
    uint32_t size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

struct ListChunk {
    char     id[4];       // "LIST"
    uint32_t size;
    char     typeId[4];   // "INFO"
};

struct InfoSubchunk {
    char     id[4];       // "ICMT" — stores original file extension
    uint32_t size;
    char     data[kMaxExtLength];
};

struct NameSubchunk {
    char     id[4];       // "INAM" — stores original file stem
    uint32_t size;
    char     data[kMaxNameLength];
};

struct DataChunk {
    char     id[4];       // "data"
    uint32_t size;
};

#pragma pack(pop)

// ─────────────────────────────────────────────────────────────
// Domain types
// ─────────────────────────────────────────────────────────────

struct EncodeParams {
    uint16_t bitsPerSample = 8;
    uint16_t numChannels   = 1;
    uint32_t sampleRate    = 44100;
};

enum class DecodeNaming { OriginalName, GenericOutput };

struct DecodeResult {
    std::vector<char> data;
    std::string       extension;
    std::string       stemName;
};

// ─────────────────────────────────────────────────────────────
// Ncurses RAII helpers
// ─────────────────────────────────────────────────────────────

void initNcurses() {
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
}

void closeNcurses() {
    endwin();
}

// ─────────────────────────────────────────────────────────────
// Argument parsing
// ─────────────────────────────────────────────────────────────

EncodeParams parseEncodeParams(int argc, char* argv[], int startFrom) {
    EncodeParams p;
    for (int i = startFrom; i + 1 < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "-BPS") { p.bitsPerSample = static_cast<uint16_t>(std::stoi(argv[++i])); }
        else if (arg == "-NC")  { p.numChannels   = static_cast<uint16_t>(std::stoi(argv[++i])); }
        else if (arg == "-SR")  { p.sampleRate    = static_cast<uint32_t>(std::stoi(argv[++i])); }
        else {
            std::cerr << "Warning: unknown argument '" << arg << "'\n";
        }
    }
    return p;
}

// ─────────────────────────────────────────────────────────────
// Encode
// ─────────────────────────────────────────────────────────────

bool encodeWAV(const std::string& inputPath,
               const std::string& outputPath,
               const EncodeParams& params) {
    std::ifstream src(inputPath, std::ios::binary);
    if (!src) { std::cerr << "encodeWAV: cannot open input: " << inputPath << '\n'; return false; }

    std::ofstream dst(outputPath, std::ios::binary);
    if (!dst) { std::cerr << "encodeWAV: cannot open output: " << outputPath << '\n'; return false; }

    // Determine raw data size
    src.seekg(0, std::ios::end);
    const auto dataSize = static_cast<uint32_t>(src.tellg());
    src.seekg(0, std::ios::beg);

    // Extract stem and extension from input path
    fs::path p(inputPath);
    const std::string stem      = p.stem().string();
    const std::string extension = p.extension().string();

    // ── FMT ──────────────────────────────────────────────────
    FmtChunk fmt{};
    std::memcpy(fmt.id, "fmt ", 4);
    fmt.size         = kFmtChunkSize;
    fmt.audioFormat  = kPcmAudioFormat;
    fmt.numChannels  = params.numChannels;
    fmt.sampleRate   = params.sampleRate;
    fmt.bitsPerSample = params.bitsPerSample;
    fmt.byteRate     = params.sampleRate * params.numChannels * (params.bitsPerSample / 8);
    fmt.blockAlign   = static_cast<uint16_t>(params.numChannels * (params.bitsPerSample / 8));

    // ── LIST / INFO ───────────────────────────────────────────
    InfoSubchunk info{};
    std::memcpy(info.id, "ICMT", 4);
    info.size = kMaxExtLength;
    std::memcpy(info.data, extension.c_str(),
                std::min(extension.size(), kMaxExtLength - 1));

    NameSubchunk name{};
    std::memcpy(name.id, "INAM", 4);
    name.size = kMaxNameLength;
    std::memcpy(name.data, stem.c_str(),
                std::min(stem.size(), kMaxNameLength - 1));

    ListChunk list{};
    std::memcpy(list.id,     "LIST", 4);
    std::memcpy(list.typeId, "INFO", 4);
    list.size = 4 + sizeof(info) + sizeof(name);

    // ── DATA ──────────────────────────────────────────────────
    DataChunk data{};
    std::memcpy(data.id, "data", 4);
    data.size = dataSize;

    // ── RIFF ──────────────────────────────────────────────────
    RiffChunk riff{};
    std::memcpy(riff.id,     "RIFF", 4);
    std::memcpy(riff.format, "WAVE", 4);
    riff.size = 4
              + sizeof(fmt)
              + sizeof(list) + sizeof(info) + sizeof(name)
              + sizeof(data) + dataSize;

    // ── Write ──────────────────────────────────────────────────
    dst.write(reinterpret_cast<char*>(&riff), sizeof(riff));
    dst.write(reinterpret_cast<char*>(&fmt),  sizeof(fmt));
    dst.write(reinterpret_cast<char*>(&list), sizeof(list));
    dst.write(reinterpret_cast<char*>(&info), sizeof(info));
    dst.write(reinterpret_cast<char*>(&name), sizeof(name));
    dst.write(reinterpret_cast<char*>(&data), sizeof(data));

    std::array<char, kBufferSize> buffer{};
    while (src.read(buffer.data(), buffer.size()) || src.gcount() > 0) {
        dst.write(buffer.data(), src.gcount());
    }

    return dst.good();
}

// ─────────────────────────────────────────────────────────────
// Decode
// ─────────────────────────────────────────────────────────────

std::optional<DecodeResult> decodeWAV(const std::string& wavPath) {
    std::ifstream file(wavPath, std::ios::binary);
    if (!file) { std::cerr << "decodeWAV: cannot open: " << wavPath << '\n'; return std::nullopt; }

    RiffChunk riff{};
    file.read(reinterpret_cast<char*>(&riff), sizeof(riff));
    if (std::strncmp(riff.id,     "RIFF", 4) != 0 ||
        std::strncmp(riff.format, "WAVE", 4) != 0) {
        std::cerr << "decodeWAV: not a valid RIFF/WAVE file\n";
        return std::nullopt;
    }

    FmtChunk fmt{};
    file.read(reinterpret_cast<char*>(&fmt), sizeof(fmt));
    if (std::strncmp(fmt.id, "fmt ", 4) != 0) {
        std::cerr << "decodeWAV: missing fmt chunk\n";
        return std::nullopt;
    }

    DecodeResult result;

    // Optional LIST/INFO chunk
    ListChunk list{};
    file.read(reinterpret_cast<char*>(&list), sizeof(list));
    if (std::strncmp(list.id, "LIST", 4) == 0) {
        InfoSubchunk info{};
        NameSubchunk name{};
        file.read(reinterpret_cast<char*>(&info), sizeof(info));
        file.read(reinterpret_cast<char*>(&name), sizeof(name));

        if (std::strncmp(info.id, "ICMT", 4) == 0) result.extension = std::string(info.data);
        if (std::strncmp(name.id, "INAM", 4) == 0) result.stemName  = std::string(name.data);

        // Read data chunk after LIST
        DataChunk data{};
        file.read(reinterpret_cast<char*>(&data), sizeof(data));
        if (std::strncmp(data.id, "data", 4) != 0) {
            std::cerr << "decodeWAV: missing data chunk after LIST\n";
            return std::nullopt;
        }
        result.data.resize(data.size);
        file.read(result.data.data(), data.size);
    } else {
        // The bytes we just read were actually the data chunk
        DataChunk data{};
        std::memcpy(&data, &list, sizeof(data));
        if (std::strncmp(data.id, "data", 4) != 0) {
            std::cerr << "decodeWAV: missing data chunk\n";
            return std::nullopt;
        }
        result.data.resize(data.size);
        file.read(result.data.data(), data.size);
    }

    return result;
}

bool writeDecoded(const DecodeResult& result, DecodeNaming naming) {
    std::string outPath;
    if (naming == DecodeNaming::OriginalName && !result.stemName.empty()) {
        outPath = result.stemName + result.extension;
    } else {
        outPath = "output" + result.extension;
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) { std::cerr << "writeDecoded: cannot open output: " << outPath << '\n'; return false; }

    out.write(result.data.data(), static_cast<std::streamsize>(result.data.size()));
    return out.good();
}

// ─────────────────────────────────────────────────────────────
// Progress bar (ncurses)
// ─────────────────────────────────────────────────────────────

static void renderProgressBar(int percent, int barWidth, int y, int x) {
    int filled = percent * barWidth / 100;
    mvprintw(y, x - 1, "[%*s]", barWidth, "");
    for (int j = 0; j < filled; ++j) mvaddch(y, x + j, '#');
    mvprintw(y + 2, x + barWidth / 2 - 4, "~>%d%%<~", percent);
}

void printProgressBar(const std::string& filename, const std::string& mode) {
    struct stat st{};
    if (stat(filename.c_str(), &st) != 0) return;

    const long size    = st.st_size;
    const int  width   = 40;
    const int  y       = LINES / 2;
    const int  x       = (COLS - width) / 2;
    const auto delayUs = static_cast<long>(
        std::max(0.5, std::log(static_cast<double>(size) + 1.0) * 0.5) * 1000.0
    );

    const char* label = (mode == "-e") ? "[>%s<] ENCODE [file -> WAV]"
                                       : "[>%s<] DECODE [WAV -> file]";

    for (int i = 0; i <= 100; ++i) {
        mvprintw(y - 2, x, label, filename.c_str());
        renderProgressBar(i, width, y, x);
        refresh();
        std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
    }

    mvprintw(y + 4, x + width / 2 - 3, "Done!");
    refresh();
    getch();
}

// ─────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    initNcurses();

    if (argc < 3) {
        mvaddstr(1, 1, "Usage: wav_tool <file> <-e|-d|-h> [options]");
        getch();
        closeNcurses();
        return 1;
    }

    const std::string filename = argv[1];
    const std::string mode     = argv[2];

    const bool isEncode = (mode == "-e" || mode == "-encode");
    const bool isDecode = (mode == "-d" || mode == "-decode");
    const bool isHelp   = (mode == "-h" || mode == "-help");

    if (!isEncode && !isDecode && !isHelp) {
        mvaddstr(1, 1, "Unknown flag. Use -e, -d, or -h.");
        getch();
        closeNcurses();
        return 1;
    }

    if (isEncode) {
        EncodeParams params = parseEncodeParams(argc, argv, 3);
        printProgressBar(filename, mode);
        const bool ok = encodeWAV(filename, "output.wav", params);
        if (!ok) { mvaddstr(1, 1, "Encode failed."); getch(); }
    }
    else if (isDecode) {
        printProgressBar(filename, mode);
        auto result = decodeWAV(filename);
        if (!result || !writeDecoded(*result, DecodeNaming::GenericOutput)) {
            mvaddstr(1, 1, "Decode failed.");
            getch();
        }
    }
    else if (isHelp) {
        mvaddstr(1, 1, "-BPS  bitsPerSample  (default: 8)");
        mvaddstr(2, 1, "-NC   numChannels    (default: 1)");
        mvaddstr(3, 1, "-SR   sampleRate     (default: 44100)");
        refresh();
        getch();
    }

    closeNcurses();
    return 0;
}
