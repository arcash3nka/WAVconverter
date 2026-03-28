/*
- нужно будет в encodeWAV добавить условия для разных названий файлов
- в целом доделать эту хуйню с именами
*/

#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <ncurses.h>
#include <unistd.h>
#include <locale>
#include <sys/stat.h>
#include <cmath>
using namespace std;

// =============================

#pragma pack(push, 1)

struct RIFF_chunk {
    char ChunkID[4];
    uint32_t ChunkSize;
    char Format[4];
};

struct FMT_chunk {
    char ChunkID[4];
    uint32_t ChunkSize;
    uint16_t AudioFormat;
    uint16_t NumChannels;
    uint32_t SampleRate;
    uint32_t ByteRate;
    uint16_t BlockAlign;
    uint16_t BitsPerSample;
};


struct LIST_chunk {
    char ChunkID[4];
    uint32_t ChunkSize;
    char TypeID[4];
};

struct INFO_subchunk {
    char SubchunkID[4];
    uint32_t SubchunkSize;
    char Data[32];
};

struct NAME_subchunk {
    char SubchunkID[4];
    uint32_t SubchunkSize;
    char Data[64];
};


struct DATA_chunk {
    char ChunkID[4];
    uint32_t ChunkSize;
};

#pragma pack(pop)

enum decodeNameMode {
    selfName,
    newName
};

struct Params {
    uint16_t BitsPerSample = 8;
    uint16_t NumChannels   = 1;
    uint32_t SampleRate    = 44100;
};

RIFF_chunk itemRIFF;
FMT_chunk itemFMT;
LIST_chunk itemLIST;
INFO_subchunk itemLIST_INFO;
NAME_subchunk itemLIST_NAME;
DATA_chunk itemDATA;
decodeNameMode nameMode = newName;
string extention;
string filename_no_ext;

//========================

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

Params parseParams(int argc, char* argv[], int startFrom) {
    Params p;
    for (int i = startFrom; i < argc - 1; i++) {
        string arg = argv[i];
        if (arg == "-BPS") p.BitsPerSample = stoi(argv[++i]);
        else if (arg == "-NC") p.NumChannels = stoi(argv[++i]);
        else if (arg == "-SR") p.SampleRate = stoi(argv[++i]);
    }
    return p;
}

// ====================================================
void encodeWAV(string inputFile, string outputFile, Params params) {
    // ============
    // =files open=
    // ============
    ifstream sourceFile(inputFile, ios::binary);
    if (!sourceFile.is_open()) return;
    
    ofstream file(outputFile, ios::binary);
    if (!file.is_open()) return;
    
    // take file-info
    sourceFile.seekg(0, ios::end);
    uint32_t dataSize = sourceFile.tellg();
    sourceFile.seekg(0, ios::beg);

    // =================
    // =create WAV info=
    // =================

    // FMT
    memcpy(itemFMT.ChunkID, "fmt ", 4);
    itemFMT.ChunkSize = 16;
    itemFMT.AudioFormat = 1;
    itemFMT.NumChannels = params.NumChannels; //моно
    itemFMT.SampleRate = params.SampleRate;
    itemFMT.BitsPerSample = params.BitsPerSample;
    itemFMT.ByteRate = params.SampleRate * params.NumChannels * (params.BitsPerSample / 8);
    itemFMT.BlockAlign = params.NumChannels * (params.BitsPerSample / 8);


    //INFO
    memcpy(itemLIST.ChunkID, "LIST", 4);
    memcpy(itemLIST.TypeID, "INFO", 4);
    
    itemLIST_INFO.SubchunkSize = sizeof(itemLIST_INFO.Data);  // 32
    itemLIST_NAME.SubchunkSize = sizeof(itemLIST_NAME.Data);

    memcpy(itemLIST_INFO.SubchunkID, "ICMT", 4);

    extention = inputFile.substr(inputFile.find_last_of('.'));
    memset(itemLIST_INFO.Data, 0, 32);
    memcpy(itemLIST_INFO.Data, extention.c_str(), extention.size());

    string fullname = inputFile.substr(inputFile.find_last_of("/\\") + 1);
    string name = fullname.substr(0, fullname.find_last_of('.'));

    memcpy(itemLIST_NAME.SubchunkID, "INAM", 4);
    memset(itemLIST_NAME.Data, 0, 64);
    memcpy(itemLIST_NAME.Data, name.c_str(), name.size());

    itemLIST.ChunkSize = 4 + sizeof(itemLIST_INFO) + sizeof(itemLIST_NAME);


    //DATA
    memcpy(itemDATA.ChunkID, "data", 4);
    itemDATA.ChunkSize = dataSize; //записывается РАЗМЕР данных снизу


    // RIFF
    memcpy(itemRIFF.ChunkID, "RIFF", 4);
    memcpy(itemRIFF.Format, "WAVE", 4);

    itemRIFF.ChunkSize = 4 + sizeof(itemFMT) + sizeof(itemLIST) + sizeof(itemLIST_INFO) + sizeof(itemLIST_NAME) + sizeof(itemDATA) + dataSize;

    // ================
    // =write WAV info=
    // ================

    file.write(reinterpret_cast<char*>(&itemRIFF), sizeof(itemRIFF));
    file.write(reinterpret_cast<char*>(&itemFMT), sizeof(itemFMT));
    file.write(reinterpret_cast<char*>(&itemLIST), sizeof(itemLIST));
    file.write(reinterpret_cast<char*>(&itemLIST_INFO), sizeof(itemLIST_INFO));
    file.write(reinterpret_cast<char*>(&itemLIST_NAME), sizeof(itemLIST_NAME));
    file.write(reinterpret_cast<char*>(&itemDATA), sizeof(itemDATA));

    char buffer[1024];
    while (sourceFile.read(buffer, sizeof(buffer)) || sourceFile.gcount() > 0) { //gcount показывает кол-во прочитанных байт
        file.write(buffer, sourceFile.gcount());
    }
    
    // =============
    // =close files=
    // =============

    cout << itemFMT.NumChannels;

    sourceFile.close();
    file.close();
}

void decodeWAV(string filename) {
    //В общем это работает так, что file.read читает определенное количество байт и записывает это все в &itemRIFF, на примере 1 случая
    ifstream file(filename, ios::binary);
    if (!file.is_open()) return;

    // RIFF
    file.read(reinterpret_cast<char*>(&itemRIFF), sizeof(itemRIFF));
    if (strncmp(itemRIFF.ChunkID, "RIFF", 4) != 0) return;
    if (strncmp(itemRIFF.Format, "WAVE", 4) != 0) return;

    // FMT
    file.read(reinterpret_cast<char*>(&itemFMT), sizeof(itemFMT));
    if (strncmp(itemFMT.ChunkID, "fmt ", 4) != 0) return;

    // LIST
    file.read(reinterpret_cast<char*>(&itemLIST), sizeof(itemLIST));
    if (strncmp(itemLIST.ChunkID, "LIST", 4) == 0) {
        file.read(reinterpret_cast<char*>(&itemLIST_INFO), sizeof(itemLIST_INFO));
        file.read(reinterpret_cast<char*>(&itemLIST_NAME), sizeof(itemLIST_NAME));
    }

    // DATA
    file.read(reinterpret_cast<char*>(&itemDATA), sizeof(itemDATA));
    if (strncmp(itemDATA.ChunkID, "data", 4) != 0) return;

    vector<char> audioData(itemDATA.ChunkSize);
    file.read(audioData.data(), itemDATA.ChunkSize);

    if (strncmp(itemLIST_INFO.SubchunkID, "ICMT", 4) == 0) {
        extention = string(itemLIST_INFO.Data);
    }
    if (strncmp(itemLIST_NAME.SubchunkID, "INAM", 4) == 0) {
        filename_no_ext = string(itemLIST_NAME.Data);
    }

    string newFileName;

    if (nameMode == newName) {
        newFileName = "output" + extention;
    }
    else if (nameMode == selfName) {
        newFileName = filename_no_ext + extention;
    }

    ofstream outputFile(newFileName, ios::binary);

    outputFile.write(audioData.data(), audioData.size());

    outputFile.close();
}

long getFileSize(string name) {
    struct stat st;
    return (stat(name.c_str(), &st) == 0) ? st.st_size : -1;
}

void printProgressBar(string filename, string flag) {
    long size = getFileSize(filename);
    if (size < 0) return;

    int width = 40;
    int y = LINES / 2;
    int x = (COLS - width) / 2;

    double delay = std::max(0.5, log(size + 1) * 0.5) / 1000;

    for (int i = 0; i <= 100; i++) {
        int filled = i * width / 100;

        (flag == "-e") ? mvprintw(y - 2, x, "[>%s<] ENCODE [file -> WAV]", filename.c_str()): mvprintw(y - 2, x, "[>%s<] DECODE [WAV -> file]", filename.c_str());

        mvprintw(y, x - 1, "[%*s]", width, "");

        for (int j = 0; j < filled; j++)
            mvaddch(y, x + j, '#');

        mvprintw(y + 2, x + width / 2 - 4, "~>%d%%<~", i);

        refresh();
        usleep(delay * 1e6);
    }

    mvprintw(y + 4, x + width / 2 - 3, "Готово!");
    refresh();
    getch();
}

int main(int argc, char* argv[]) {
    initNcurses();
    
    if (argc < 3) {
        mvaddstr(1, 1, "Not enough arguments");
        closeNcurses();
        return 1;
    }

    string filename = argv[1];
    string flag = argv[2];
    
    if (flag != "-e" && flag != "-d" && flag != "-encode" && flag != "-decode" && flag != "-h" && flag != "-help") {
        mvaddstr(1, 1, "Incorrect flag");
        closeNcurses();
        return 1;
    }

    if (flag == "-e" || flag == "-encode") {
        Params param = parseParams(argc, argv, 3);
        printProgressBar(filename, flag);
        encodeWAV(filename, "output.wav", param);
    }
    else if (flag == "-d" || flag == "-decode") {
        printProgressBar(filename, flag);
        decodeWAV(filename);
    }
    else if (flag == "-h" || flag == "-help") {
        mvaddstr(1,1,"-BPC = bitsPerSample");
        mvaddstr(2,1,"-NC = numChannels");
        mvaddstr(3,1,"-SR = sampleRate");
        refresh();
        getch();
    }
    
    // комент для проверки
    // ватафак назафак

    closeNcurses();
}
