// =====================================================================
//  AVR CLI Studio - Console Emulator Core (emu.cpp)
//  C++ replacement of the AVR Studio 4 simulator (TUI dashboard).
//  Build (host): g++ -O2 -std=c++17 -Wall -Wextra -o emu.exe emu.cpp
//  Config source: echo.ctos + Procs/<Chip>.proc  (README sec. 8, 13.1)
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ---------------------------------------------------------------------
// CPU state
// ---------------------------------------------------------------------
static constexpr uint32_t FLASH_END = 0x2000;   // 8KB (ATmega8)
static constexpr uint32_t DMEM_SIZE = 0x460;    // R0-31(0-1F) + I/O(20-5F) + SRAM(60-45F)
static constexpr uint16_t RAM_END   = 0x045F;

static uint8_t  g_flash[FLASH_END] = {0xFF};
static std::vector<uint8_t> g_dmem(DMEM_SIZE, 0);
static uint16_t g_pc = 0;           // in WORDS
static uint64_t g_cycles = 0;
static bool     g_loaded = false;
static bool     g_running = false;
static bool     g_quiet   = false;

// SREG flags
static struct { int I,T,H,S,V,N,Z,C; } g_f = {};

// GPIO external stimulus: per port forced mask + forced value
static struct { uint8_t mask, level; } g_ext[3] = {{0,0},{0,0},{0,0}};
// port index: 0=B,1=C,2=D ; IO addresses
static struct { int pin, ddr, port; } g_pio[3] = {{0x16,0x17,0x18},{0x13,0x14,0x15},{0x10,0x11,0x12}};
static const char* g_pname[3] = {"B","C","D"};

// config
static std::string g_chip = "ATmega8A";
static std::string g_mcu  = "atmega8";
static uint32_t    g_fcpu = 8000000;

// breakpoints: index by PC (word). value = id
static std::map<uint16_t,int> g_bp;
static std::map<uint16_t,int>::size_type g_bpSerial = 1;

// watch: {name, sram addr}
struct Watch { std::string name; uint16_t addr; };
static std::vector<Watch> g_watch;

// previous frame for delta marking
static std::map<std::string,int> g_prev;
static bool g_prevValid = false;

static std::string g_root;   // project root (parent of build/)
static std::string g_elf;    // path to elf file

// ---------------------------------------------------------------------
// Console color helpers (16-color buffer, works PS1.1-era consoles)
// ---------------------------------------------------------------------
static HANDLE g_con = nullptr;
enum CLR { GRAY=7, RED=12, GREEN=10, YELLOW=14, CYAN=11, DGREEN=2, DCYAN=3, DGRAY=8, WHITE=15, MAGENTA=13 };
static void setColor(WORD c) { if(g_con) SetConsoleTextAttribute(g_con, c); }
static void wr(const std::string& s, WORD c = GRAY, bool nl = true) {
    if (g_quiet) return;
    setColor(c);
    fputs(s.c_str(), stdout);
    if (nl) fputc('\n', stdout);
    fflush(stdout);
    setColor(GRAY);
}
static std::string fmt(const char* f, ...) {
    char buf[256]; va_list ap; va_start(ap, f);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, f, ap);
    va_end(ap); return buf;
}

// forward decls
static std::string lower(std::string s);
static std::string upper(std::string s);
static std::string joinTokens(const std::vector<std::string>& v);

// ---------------------------------------------------------------------
// Config loading (UTF-8 without BOM, key = value; [] sections skipped)
// ---------------------------------------------------------------------
static std::map<std::string,std::string> readPairs(const std::string& path) {
    std::map<std::string,std::string> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        if (line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq+1);
        k.erase(k.find_last_not_of(" \t")+1);
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t\r")+1);
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        if (!v.empty() && v.back()=='\r') v.pop_back();
        out[k] = v;
    }
    return out;
}

static void loadEnv() {
    auto cfg = readPairs(g_root + "/echo.ctos");
    auto it  = [&cfg](const char* k){ auto i = cfg.find(k); return i!=cfg.end()? i->second : std::string(); };
    if (!it("mcu_target").empty())       g_mcu  = it("mcu_target");
    if (!it("processorprofile").empty()) g_chip = it("processorprofile");
    if (!it("f_cpu").empty())  g_fcpu = (uint32_t)atoll(it("f_cpu").c_str());  // 8000000UL -> atol ok

    std::string prof = g_root + "/Procs/" + g_chip + ".proc";
    auto p = readPairs(prof);
    auto pit = [&p](const char* k){ auto i = p.find(k); return i!=p.end()? i->second : std::string(); };
    if (!pit("name").empty())     g_chip = pit("name");
    if (!pit("ramend").empty())   { /* RAM fixed for atmega8 */ }
}

// ---------------------------------------------------------------------
// Data memory + GPIO logic (README sec.8.1 rule: DDR=0 & PORT=1 -> PIN=1)
// ---------------------------------------------------------------------
static uint8_t readDmem(int a) {
    if (a < 0 || a >= (int)DMEM_SIZE) return 0;
    for (int i = 0; i < 3; i++) {
        int pinRam = 0x20 + g_pio[i].pin;
        if (a == pinRam) {
            uint8_t ddr = g_dmem[0x20 + g_pio[i].ddr];
            uint8_t prt = g_dmem[0x20 + g_pio[i].port];
            uint8_t res = 0;
            for (int b = 0; b < 8; b++) {
                int out = (ddr >> b) & 1;
                if (out) { if ((prt >> b) & 1) res |= (1<<b); }
                else {
                    if ((g_ext[i].mask >> b) & 1) { if ((g_ext[i].level >> b) & 1) res |= (1<<b); }
                    else if ((prt >> b) & 1) res |= (1<<b);  // pull-up
                }
            }
            return res;
        }
    }
    if (a == 0x20 + 0x3F) { // SREG
        return (g_f.I<<7)|(g_f.T<<6)|(g_f.H<<5)|(g_f.S<<4)|(g_f.V<<3)|(g_f.N<<2)|(g_f.Z<<1)|(g_f.C);
    }
    return g_dmem[a];
}
static void writeDmem(int a, int val) {
    val &= 0xFF;
    if (a == 0x20 + 0x3F) {
        g_f.I=(val>>7)&1; g_f.T=(val>>6)&1; g_f.H=(val>>5)&1; g_f.S=(val>>4)&1;
        g_f.V=(val>>3)&1; g_f.N=(val>>2)&1; g_f.Z=(val>>1)&1; g_f.C=val&1;
        return;
    }
    if (a >= 0 && a < (int)DMEM_SIZE) g_dmem[a] = (uint8_t)val;
}

// ---------------------------------------------------------------------
// Stack
// ---------------------------------------------------------------------
static void pushByte(int v) {
    uint16_t sp = g_dmem[0x5D] | (g_dmem[0x5E] << 8);
    sp--; g_dmem[0x5D] = sp & 0xFF; g_dmem[0x5E] = (sp >> 8) & 0xFF;
    if (sp < DMEM_SIZE) g_dmem[sp] = (uint8_t)(v & 0xFF);
}
static int popByte() {
    uint16_t sp = g_dmem[0x5D] | (g_dmem[0x5E] << 8);
    int v = g_dmem[sp]; sp++;
    g_dmem[0x5D] = sp & 0xFF; g_dmem[0x5E] = (sp >> 8) & 0xFF;
    return v;
}
static void pushWord(int v) { pushByte((v >> 8) & 0xFF); pushByte(v & 0xFF); }
static int  popWord() { int lo = popByte(); int hi = popByte(); return lo | (hi << 8); }

// ---------------------------------------------------------------------
// Flag rules
// ---------------------------------------------------------------------
static void setAddFlags(int a, int b, int r, int cin) {
    a &= 0xFF; b &= 0xFF; r &= 0xFF;
    int c = (r < ((a + b + cin) & 0x1FF)) ? 1 : 0;
    c = ((a + b + cin) > 0xFF) ? 1 : 0;
    int h = ((a & 8) && ((b & 8) || !(r & 8))) | (!(a & 8) && (b & 8) && (r & 8));
    int n = (r >> 7) & 1, v = n ^ c;
    g_f.C = c; g_f.H = h; g_f.N = n; g_f.V = v; g_f.S = n ^ v; g_f.Z = (r == 0);
}
static void setSubFlags(int a, int b, int r, int bin, bool sticky) {
    a &= 0xFF; b &= 0xFF; r &= 0xFF;
    int c = ((a - b - bin) < 0) ? 1 : 0;
    int h = (!(a & 8) && (b & 8)) | ((b & 8) && !(r & 8)) | (!(r & 8) && !(a & 8));
    int n = (r >> 7) & 1, v = n ^ c;
    g_f.C = c; g_f.H = h; g_f.N = n; g_f.V = v; g_f.S = n ^ v;
    g_f.Z = sticky ? (int)((r == 0) && g_f.Z) : (int)(r == 0);
}
static void setLogicFlags(int r) {
    r &= 0xFF;
    int n = (r >> 7) & 1;
    g_f.N = n; g_f.V = 0; g_f.S = n; g_f.Z = (r == 0);
}
static void setShiftFlags(int r, int cout) {
    r &= 0xFF;
    int n = (r >> 7) & 1, v = n ^ cout;
    g_f.C = cout; g_f.N = n; g_f.V = v; g_f.S = n ^ v; g_f.Z = (r == 0);
}

// ---------------------------------------------------------------------
// ELF32 loading + symbol lookup
// ---------------------------------------------------------------------
struct Elf { std::vector<uint8_t> buf; };
static bool loadElfSys(const std::string& path, std::string& err);
static bool findSymbol(const std::string& name, uint32_t& valueOut);

static uint32_t rD(const std::vector<uint8_t>& b, int o) {
    return b[o] | (b[o+1]<<8) | (b[o+2]<<16) | (b[o+3]<<24);
}
static uint16_t rW(const std::vector<uint8_t>& b, int o) { return b[o] | (b[o+1]<<8); }

static bool loadElfSys(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52 || b[0] != 0x7F || b[1] != 'E') { err = "not an ELF file"; return false; }

    memset(g_flash, 0xFF, sizeof(g_flash));
    std::fill(g_dmem.begin(), g_dmem.end(), 0);
    for (int i = 0; i < 3; i++) { g_ext[i].mask = 0; g_ext[i].level = 0; }
    g_f = {}; g_pc = 0; g_cycles = 0; g_running = false;
    g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END >> 8) & 0xFF;

    uint32_t phoff = rD(b, 28);
    uint16_t phnum = rW(b, 44);
    int loaded = 0;
    for (int i = 0; i < (int)phnum; i++) {
        int o = (int)phoff + i * 32;
        if (o + 32 > (int)b.size()) break;
        if (rD(b, o + 0) != 1) continue;               // PT_LOAD
        uint32_t pOff = rD(b, o + 4), pAddr = rD(b, o + 8), pSize = rD(b, o + 16);
        for (uint32_t j = 0; j < pSize; j++) {
            uint32_t a = pAddr + j;
            if (a < FLASH_END && (pOff + j) < b.size()) g_flash[a] = b[pOff + j];
        }
        loaded++;
    }
    if (!loaded) { err = "no PT_LOAD segments"; return false; }
    g_loaded = true;
    return true;
}
static bool findSymbol(const std::string& name, uint32_t& valueOut) {
    std::ifstream f(g_elf, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52 || b[0] != 0x7F) return false;
    uint32_t shoff = rD(b, 32); uint16_t shnum = rW(b, 48);
    uint32_t symOff = 0, symSize = 0, symEnt = 16; int symLink = -1;
    for (int i = 0; i < (int)shnum; i++) {
        int o = (int)shoff + i * 40;
        if (o + 40 > (int)b.size()) break;
        if (rD(b, o + 4) == 2) {                       // SHT_SYMTAB
            symOff = rD(b, o + 16); symSize = rD(b, o + 20);
            symLink = (int)rD(b, o + 24); symEnt = rD(b, o + 36);
            if (!symEnt) symEnt = 16;
        }
    }
    if (!symOff || symLink < 0) return false;
    int sHo = (int)shoff + symLink * 40;
    uint32_t strOff = rD(b, sHo + 16);
    uint32_t n = symSize / symEnt;
    for (uint32_t i = 0; i < n; i++) {
        int o = (int)symOff + (int)(i * symEnt);
        if (o + 16 > (int)b.size()) break;
        uint32_t nameOff = rD(b, o + 0);
        std::string sb; uint32_t p = strOff + nameOff;
        while (p < b.size() && b[p] != 0) { sb += (char)b[p]; ++p; }
        if (sb == name) { valueOut = rD(b, o + 4); return true; }
    }
    return false;
}

// ---------------------------------------------------------------------
// Fetch / decode / execute
// ---------------------------------------------------------------------
static int getChipFlag(int s) {
    switch (s%8) { case 0: return g_f.C; case 1: return g_f.Z; case 2: return g_f.N;
        case 3: return g_f.V; case 4: return g_f.S; case 5: return g_f.H; case 6: return g_f.T; }
    return g_f.I;
}
static void setChipFlag(int s, int v) {
    switch (s%8) { case 0: g_f.C = v; break; case 1: g_f.Z = v; break; case 2: g_f.N = v; break;
        case 3: g_f.V = v; break; case 4: g_f.S = v; break; case 5: g_f.H = v; break; case 6: g_f.T = v; break;
        default: g_f.I = v; break; }
}
static uint16_t flashWord(int w) { int o = w*2; return (uint16_t)(g_flash[o] | (g_flash[o+1]<<8)); }

// returns false on failure / stop. Sets opOut, cyOut
static bool execOne(uint16_t& opOut, int& cyOut) {
    if (!g_loaded) { wr("! No program loaded. Use: emu start", RED); return false; }
    int pcB = g_pc * 2;
    if (pcB >= (int)FLASH_END) { g_running = false; return false; }
    uint16_t op = flashWord(g_pc);
    int nextPC = g_pc + 1, cy = 1;
    int rd, rr, k;

    enum { NON = 0 };
    if (op == 0x0000) { }                                          // NOP
    else if (op == 0x9698) {                                       // BREAK
        wr(fmt(">> BREAK opcode at PC=0x%04X. Stopping.", g_pc), YELLOW);
        g_running = false; return false;
    }
    else if ((op & 0xFF00) == 0x0100) {                            // MOVW (even pairs)
        rd = ((op>>4)&0xF)*2; rr = (op&0xF)*2;
        g_dmem[rd] = g_dmem[rr]; g_dmem[rd+1] = g_dmem[rr+1];
    }
    else if ((op & 0xFC00) == 0x9C00) {                            // MUL Rd,Rr (unsigned)
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        uint16_t prod = (uint16_t)((uint16_t)g_dmem[rd] * (uint16_t)g_dmem[rr]);
        g_dmem[0] = prod & 0xFF; g_dmem[1] = (prod>>8)&0xFF;
        g_f.C = (prod>>15)&1; g_f.Z = (prod == 0); cy = 2;
    }
    else if ((op & 0xFF00) == 0x0200) {                            // MULS Rd,Rr (both signed)
        rd = 16 + ((op>>4)&0xF); rr = 16 + (op&0xF);
        int p = (int8_t)g_dmem[rd] * (int8_t)g_dmem[rr];
        g_dmem[0] = p & 0xFF; g_dmem[1] = (p>>8)&0xFF;
        g_f.C = (p>>15)&1; g_f.Z = ((p & 0xFFFF) == 0); cy = 2;
    }
    else if ((op & 0xFF80) == 0x0300) {                            // MULSU/FMUL/FMULS/FMULSU
        rd = 16 + ((op>>4)&7); rr = 16 + (op&7);
        int b7 = (op>>7)&1, b3 = (op>>3)&1;
        int sat = (int8_t)g_dmem[rd], rat = g_dmem[rr];
        uint32_t p;
        switch ((b7<<1)|b3) {
            case 0: p = (uint32_t)(sat * rat); break;                          // MULSU  s*u
            case 1: p = (uint32_t)g_dmem[rd] * g_dmem[rr]; break;               // FMUL   u*u
            case 2: p = (uint32_t)(sat * (int8_t)rat); break;                  // FMULSU s*u (<<1)
            case 3: p = (uint32_t)(sat * (int8_t)rat); break;                  // FMULS  s*s (<<1)
        }
        if (b7) p <<= 1;                                   // FMUL-family: result shifted left
        g_dmem[0] = p & 0xFF; g_dmem[1] = (p>>8)&0xFF;
        g_f.C = (p>>16)&1; g_f.Z = ((p & 0xFFFF) == 0); cy = 2;
    }
    else if ((op & 0xFC00) == 0x0C00) {                            // ADD
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int a = g_dmem[rd], r = a + g_dmem[rr];
        setAddFlags(a, g_dmem[rr], r, 0); g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xFC00) == 0x1C00) {                            // ADC
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int a = g_dmem[rd], r = a + g_dmem[rr] + g_f.C;
        setAddFlags(a, g_dmem[rr], r, g_f.C); g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xFC00) == 0x1800) {                            // SUB
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int a = g_dmem[rd], b2 = g_dmem[rr], r = a - b2;
        setSubFlags(a, b2, r, 0, false); g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xFC00) == 0x0800) {                            // SBC
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int a = g_dmem[rd], b2 = g_dmem[rr], r = a - b2 - g_f.C;
        setSubFlags(a, b2, r, g_f.C, true); g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xFC00) == 0x1400) {                            // CP
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        setSubFlags(g_dmem[rd], g_dmem[rr], g_dmem[rd]-g_dmem[rr], 0, false);
    }
    else if ((op & 0xFC00) == 0x0400) {                            // CPC
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        setSubFlags(g_dmem[rd], g_dmem[rr], g_dmem[rd]-g_dmem[rr]-g_f.C, g_f.C, true);
    }
    else if ((op & 0xFC00) == 0x1000) {                            // CPSE
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        if (g_dmem[rd] == g_dmem[rr]) { nextPC = g_pc + 2; cy = 2; }
    }
    else if ((op & 0xFC00) == 0x2000) {                            // AND
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int r = g_dmem[rd] & g_dmem[rr]; g_dmem[rd] = (uint8_t)r; setLogicFlags(r);
    }
    else if ((op & 0xFC00) == 0x2400) {                            // EOR
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int r = g_dmem[rd] ^ g_dmem[rr]; g_dmem[rd] = (uint8_t)r; setLogicFlags(r);
    }
    else if ((op & 0xFC00) == 0x2800) {                            // OR
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF);
        int r = g_dmem[rd] | g_dmem[rr]; g_dmem[rd] = (uint8_t)r; setLogicFlags(r);
    }
    else if ((op & 0xFC00) == 0x2C00) {                            // MOV
        rd = (op>>4)&0x1F; rr = ((op>>5)&0x10) | (op&0xF); g_dmem[rd] = g_dmem[rr];
    }
    else if ((op & 0xF000) == 0x3000) {                            // CPI
        rd = 16 + ((op>>4)&0xF); k = (op&0xF) | (((op>>4)&0xF0));
        setSubFlags(g_dmem[rd], k, g_dmem[rd]-k, 0, false);
    }
    else if ((op & 0xF000) == 0x4000) {                            // SBCI
        rd = 16 + ((op>>4)&0xF); k = (op&0xF) | (((op>>4)&0xF0));
        int a = g_dmem[rd], r = a - k - g_f.C;
        setSubFlags(a, k, r, g_f.C, true); g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xF000) == 0x5000) {                            // SUBI
        rd = 16 + ((op>>4)&0xF); k = (op&0xF) | (((op>>4)&0xF0));
        int a = g_dmem[rd], r = a - k;
        setSubFlags(a, k, r, 0, false); g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xF000) == 0x6000) {                            // ORI
        rd = 16 + ((op>>4)&0xF); k = (op&0xF) | (((op>>4)&0xF0));
        int r = g_dmem[rd] | k; g_dmem[rd] = (uint8_t)r; setLogicFlags(r);
    }
    else if ((op & 0xF000) == 0x7000) {                            // ANDI
        rd = 16 + ((op>>4)&0xF); k = (op&0xF) | (((op>>4)&0xF0));
        int r = g_dmem[rd] & k; g_dmem[rd] = (uint8_t)r; setLogicFlags(r);
    }
    else if ((op & 0xF000) == 0xE000) {                            // LDI
        rd = 16 + ((op>>4)&0xF); k = (op&0xF) | (((op>>4)&0xF0));
        g_dmem[rd] = (uint8_t)k;
    }
    else if ((op & 0xFF00) == 0x9600) {                            // ADIW
        rd = 24 + (((op>>4)&3) * 2); k = (op&0xF) | ((op>>2)&0x30);
        int lo = g_dmem[rd], hi = g_dmem[rd+1], orig = lo | (hi<<8);
        uint32_t res = (uint32_t)((orig + k) & 0xFFFF);
        int n = (res>>15)&1;
        g_f.N = n; g_f.V = ((orig&0x8000) && !(res&0x8000)) ? 1:0; g_f.S = n ^ g_f.V;
        g_f.C = (orig + k > 0xFFFF) ? 1 : 0;
        g_f.Z = (res == 0);
        g_dmem[rd] = res & 0xFF; g_dmem[rd+1] = (res>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFF00) == 0x9700) {                            // SBIW
        rd = 24 + (((op>>4)&3) * 2); k = (op&0xF) | ((op>>2)&0x30);
        int lo = g_dmem[rd], hi = g_dmem[rd+1], orig = lo | (hi<<8);
        uint32_t res = (uint32_t)((orig - k) & 0xFFFF);
        int n = (res>>15)&1;
        g_f.N = n; g_f.V = (!(orig&0x8000) && (res&0x8000)) ? 1:0;
        g_f.S = n ^ g_f.V; g_f.C = ((uint32_t)orig < (uint32_t)k) ? 1 : 0;
        g_f.Z = (res == 0);
        g_dmem[rd] = res & 0xFF; g_dmem[rd+1] = (res>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xF800) == 0xB000) {                            // IN
        int A = (op & 0xF) | (((op>>8)&3)<<4);
        rd = (op>>4)&0x1F;
        g_dmem[rd] = readDmem(0x20 + A);
    }
    else if ((op & 0xF800) == 0xB800) {                            // OUT
        int A = (op & 0xF) | (((op>>8)&3)<<4);
        rr = (op>>4)&0x1F;
        writeDmem(0x20 + A, g_dmem[rr]);
    }
    else if ((op & 0xFF00) == 0x9800) {                          // CBI
        int A = (op>>3)&0x1F, bt = op&7;
        int addr = 0x20 + A; writeDmem(addr, readDmem(addr) & ~(1<<bt)); cy = 2;
    }
    else if ((op & 0xFF00) == 0x9A00) {                          // SBI
        int A = (op>>3)&0x1F, bt = op&7;
        int addr = 0x20 + A; writeDmem(addr, readDmem(addr) | (1<<bt)); cy = 2;
    }
    else if ((op & 0xFF00) == 0x9900) {                          // SBIC
        int A = (op>>3)&0x1F, bt = op&7;
        if (!((readDmem(0x20+A) >> bt) & 1)) { nextPC = g_pc + 2; cy = 2; }
    }
    else if ((op & 0xFF00) == 0x9B00) {                          // SBIS
        int A = (op>>3)&0x1F, bt = op&7;
        if ((readDmem(0x20+A) >> bt) & 1) { nextPC = g_pc + 2; cy = 2; }
    }
    else if ((op & 0xF000) == 0xC000) {                            // RJMP
        int kk = op & 0xFFF; if (kk >= 0x800) kk -= 0x1000;
        nextPC = g_pc + 1 + kk; cy = 2;
    }
    else if ((op & 0xF000) == 0xD000) {                            // RCALL
        int kk = op & 0xFFF; if (kk >= 0x800) kk -= 0x1000;
        pushWord(g_pc + 1); nextPC = g_pc + 1 + kk; cy = 3;
    }
    else if (op == 0x9508) { nextPC = popWord(); cy = 4; }        // RET
    else if (op == 0x9518) { g_f.I = 1; nextPC = popWord(); cy = 4; }  // RETI
    else if (op == 0x9509) {                                     // ICALL
        pushWord(g_pc + 1);
        uint16_t z = g_dmem[0x1A] | (g_dmem[0x1B] << 8);
        nextPC = z / 2; cy = 3;
    }
    else if (op == 0x9409) {                                     // IJMP
        uint16_t z = g_dmem[0x1A] | (g_dmem[0x1B] << 8);
        nextPC = z / 2; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9000) {                          // LDS Rd, k (2 words)
        rd = (op>>4)&0x1F; uint16_t kk = flashWord(g_pc + 1);
        g_dmem[rd] = readDmem(kk); nextPC = g_pc + 2; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9200) {                          // STS k, Rr (2 words)
        rr = (op>>4)&0x1F; uint16_t kk = flashWord(g_pc + 1);
        writeDmem(kk, g_dmem[rr]); nextPC = g_pc + 2; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x900C) {                          // LD Rd, X
        rd = (op>>4)&0x1F; uint16_t x = g_dmem[0x1A] | (g_dmem[0x1B]<<8);
        g_dmem[rd] = readDmem(x); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x900D) {                          // LD Rd, X+
        rd = (op>>4)&0x1F; uint16_t x = g_dmem[0x1A] | (g_dmem[0x1B]<<8);
        g_dmem[rd] = readDmem(x); x++; g_dmem[0x1A] = x&0xFF; g_dmem[0x1B] = (x>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9009) {                          // LD Rd, Y+
        rd = (op>>4)&0x1F; uint16_t y = g_dmem[0x18] | (g_dmem[0x19]<<8);
        g_dmem[rd] = readDmem(y); y++; g_dmem[0x18] = y&0xFF; g_dmem[0x19] = (y>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x900A) {                          // LD Rd, -Y
        rd = (op>>4)&0x1F; uint16_t y = (g_dmem[0x18] | (g_dmem[0x19]<<8)) - 1;
        g_dmem[0x18] = y&0xFF; g_dmem[0x19] = (y>>8)&0xFF; g_dmem[rd] = readDmem(y); cy = 2;
    }
    else if ((op & 0xD200) == 0x8000) {                          // LDD Rd, Y+q / Z+q
        rd = (op>>4)&0x1F; int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        if (op & 8) { uint16_t y = g_dmem[0x18] | (g_dmem[0x19]<<8); g_dmem[rd] = readDmem(y + q); }
        else        { uint16_t z = g_dmem[0x1A] | (g_dmem[0x1B]<<8); g_dmem[rd] = readDmem(z + q); }
        cy = 2;
    }
    else if ((op & 0xFE0F) == 0x900F) {                          // POP
        rd = (op>>4)&0x1F; g_dmem[rd] = (uint8_t)popByte(); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9004) {                          // LPM Rd, Z
        rd = (op>>4)&0x1F; uint16_t z = g_dmem[0x1A] | (g_dmem[0x1B]<<8);
        g_dmem[rd] = z < FLASH_END ? g_flash[z] : 0xFF; cy = 3;
    }
    else if ((op & 0xFE0F) == 0x9005) {                          // LPM Rd, Z+
        rd = (op>>4)&0x1F; uint16_t z = g_dmem[0x1A] | (g_dmem[0x1B]<<8);
        g_dmem[rd] = z < FLASH_END ? g_flash[z] : 0xFF; z++;
        g_dmem[0x1A] = z&0xFF; g_dmem[0x1B] = (z>>8)&0xFF; cy = 3;
    }
    else if ((op & 0xFE0F) == 0x920C) {                          // ST X, Rr
        rr = (op>>4)&0x1F; uint16_t x = g_dmem[0x1A] | (g_dmem[0x1B]<<8);
        writeDmem(x, g_dmem[rr]); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x920D) {                          // ST X+, Rr
        rr = (op>>4)&0x1F; uint16_t x = g_dmem[0x1A] | (g_dmem[0x1B]<<8);
        writeDmem(x, g_dmem[rr]); x++; g_dmem[0x1A] = x&0xFF; g_dmem[0x1B] = (x>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9209) {                          // ST Y+, Rr
        rr = (op>>4)&0x1F; uint16_t y = g_dmem[0x18] | (g_dmem[0x19]<<8);
        writeDmem(y, g_dmem[rr]); y++; g_dmem[0x18] = y&0xFF; g_dmem[0x19] = (y>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x920A) {                          // ST -Y, Rr
        rr = (op>>4)&0x1F; uint16_t y = (g_dmem[0x18] | (g_dmem[0x19]<<8)) - 1;
        g_dmem[0x18] = y&0xFF; g_dmem[0x19] = (y>>8)&0xFF; writeDmem(y, g_dmem[rr]); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x920E) {                          // ST -Z, Rr
        rr = (op>>4)&0x1F; uint16_t z = (g_dmem[0x1A] | (g_dmem[0x1B]<<8)) - 1;
        g_dmem[0x1A] = z&0xFF; g_dmem[0x1B] = (z>>8)&0xFF; writeDmem(z, g_dmem[rr]); cy = 2;
    }
    else if ((op & 0xD200) == 0x8200) {                          // STD Y+q / Z+q, Rr
        rr = (op>>4)&0x1F; int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        if (op & 8) { uint16_t y = g_dmem[0x18] | (g_dmem[0x19]<<8); writeDmem(y + q, g_dmem[rr]); }
        else        { uint16_t z = g_dmem[0x1A] | (g_dmem[0x1B]<<8); writeDmem(z + q, g_dmem[rr]); }
        cy = 2;
    }
    else if ((op & 0xFE0F) == 0x920F) {                          // PUSH
        rr = (op>>4)&0x1F; pushByte(g_dmem[rr]); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9400) {                          // COM
        rd = (op>>4)&0x1F; int r = ~g_dmem[rd] & 0xFF;
        g_dmem[rd] = (uint8_t)r; setLogicFlags(r); g_f.C = 1;
    }
    else if ((op & 0xFE0F) == 0x9401) {                          // NEG
        rd = (op>>4)&0x1F; int a = g_dmem[rd], r = 0 - a;
        setSubFlags(0, a, r, 0, false); g_f.H = (r & 8) ? 1 : 0;
        g_dmem[rd] = (uint8_t)r;
    }
    else if ((op & 0xFE0F) == 0x9402) {                          // SWAP
        rd = (op>>4)&0x1F; int v = g_dmem[rd];
        g_dmem[rd] = (uint8_t)(((v<<4)|(v>>4)) & 0xFF);
    }
    else if ((op & 0xFE0F) == 0x9403) {                          // INC
        rd = (op>>4)&0x1F; int r = (g_dmem[rd]+1)&0xFF; g_dmem[rd] = (uint8_t)r;
        g_f.N = (r>>7)&1; g_f.V = (r == 0x80); g_f.S = g_f.N ^ g_f.V; g_f.Z = (r == 0);
    }
    else if ((op & 0xFE0F) == 0x940A) {                          // DEC
        rd = (op>>4)&0x1F; int r = (g_dmem[rd]-1)&0xFF; g_dmem[rd] = (uint8_t)r;
        g_f.N = (r>>7)&1; g_f.V = (r == 0x7F); g_f.S = g_f.N ^ g_f.V; g_f.Z = (r == 0);
    }
    else if ((op & 0xFE0F) == 0x9405) {                          // ASR
        rd = (op>>4)&0x1F; int v = g_dmem[rd], cout = v&1, r = (v>>1)|(v&0x80);
        g_dmem[rd] = (uint8_t)r; setShiftFlags(r, cout);
    }
    else if ((op & 0xFE0F) == 0x9406) {                          // LSR
        rd = (op>>4)&0x1F; int v = g_dmem[rd], cout = v&1, r = v>>1;
        g_dmem[rd] = (uint8_t)r; setShiftFlags(r, cout);
    }
    else if ((op & 0xFE0F) == 0x9407) {                          // ROR
        rd = (op>>4)&0x1F; int v = g_dmem[rd], cout = v&1, cin = g_f.C;
        int r = (v>>1) | ((cin<<7)&0x80);
        g_dmem[rd] = (uint8_t)r; setShiftFlags(r, cout);
    }
    else if ((op & 0xFF0F) == 0x9408) {                          // BSET s (SEC/SEZ/SEN/...)
        setChipFlag((op>>4)&7, 1);
    }
    else if ((op & 0xFF0F) == 0x9488) {                          // BCLR s (CLC/CLU/CLN/...)
        setChipFlag((op>>4)&7, 0);
    }
    else if (op == 0x9588) { cy = 1; }                           // SLEEP (ignored)
    else if (op == 0x95A8) { cy = 1; }                           // WDR (ignored)
    else if ((op & 0xFE0F) == 0x940C) {                          // JMP (2 words)
        nextPC = flashWord(g_pc + 1) / 2; cy = 3;
    }
    else if ((op & 0xFE0F) == 0x940E) {                          // CALL (2 words)
        pushWord(g_pc + 2); nextPC = flashWord(g_pc + 1) / 2; cy = 4;
    }
    else if ((op & 0xFE08) == 0xFC00) {                          // SBRC Rr,b : skip if bit==0
        rr = (op>>4)&0x1F;
        if (!((g_dmem[rr] >> (op&7)) & 1)) { nextPC = g_pc + 2; cy = 2; }
    }
    else if ((op & 0xFE08) == 0xFE00) {                          // SBRS Rr,b : skip if bit==1
        rr = (op>>4)&0x1F;
        if ((g_dmem[rr] >> (op&7)) & 1) { nextPC = g_pc + 2; cy = 2; }
    }
    else if ((op & 0xFE08) == 0xFA00) {                          // BST Rd,b -> T
        rd = (op>>4)&0x1F; g_f.T = (g_dmem[rd] >> (op&7)) & 1;
    }
    else if ((op & 0xFE08) == 0xF800) {                          // BLD Rd,b <- T
        rd = (op>>4)&0x1F; int b = op&7;
        g_dmem[rd] = (uint8_t)((g_dmem[rd] & ~(1<<b)) | (g_f.T << b));
    }
    else if ((op & 0xFC00) == 0xF000) {                            // BRBS
        int s = op & 7, kk = (op>>3)&0x7F; if (kk >= 0x40) kk -= 0x80;
        if (getChipFlag(s)) { nextPC = g_pc + 1 + kk; cy = 2; }
    }
    else if ((op & 0xFC00) == 0xF400) {                            // BRBC
        int s = op & 7, kk = (op>>3)&0x7F; if (kk >= 0x40) kk -= 0x80;
        if (!getChipFlag(s)) { nextPC = g_pc + 1 + kk; cy = 2; }
    }
    else {
        wr(fmt("!! Unsupported opcode 0x%04X at PC=0x%04X. Stopping.", op, g_pc), RED);
        g_running = false; return false;
    }

    g_pc = (uint16_t)nextPC;
    g_cycles += cy;
    opOut = op; cyOut = cy;
    return true;
}

// ---------------------------------------------------------------------
// Light disassembler
// ---------------------------------------------------------------------
static std::string decodeOp(uint16_t op) {
    char b[96];
    int d;
    if (op == 0x0000) return "NOP";
    if ((op & 0xFF00) == 0x0100) { snprintf(b, sizeof b, "MOVW R%u, R%u", ((op>>4)&0xF)*2, (op&0xF)*2); return b; }
    if ((op & 0xFC00) == 0x9C00) { snprintf(b, sizeof b, "MUL  R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b; }
    if ((op & 0xFF00) == 0x0200) { snprintf(b, sizeof b, "MULS R%u, R%u", 16+((op>>4)&0xF), 16+(op&0xF)); return b; }
    if ((op & 0xFF80) == 0x0300) {
        static const char* fm[4] = {"MULSU","FMUL","FMULSU","FMULS"};
        snprintf(b, sizeof b, "%s R%u, R%u", fm[((op>>7)&1)<<1 | (op>>3)&1], 16+((op>>4)&7), 16+(op&7)); return b;
    }
    if ((op & 0xFE0F) == 0x9000) { snprintf(b, sizeof b, "LDS  R%u, 0x%04X", (op>>4)&0x1F, flashWord(g_pc+1)); return b; }
    if ((op & 0xFE0F) == 0x9200) { snprintf(b, sizeof b, "STS  0x%04X, R%u", flashWord(g_pc+1), (op>>4)&0x1F); return b; }
    if ((op & 0xD200) == 0x8000) {
        int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        snprintf(b, sizeof b, "%s R%u, %c+%d", (op&8)?"LDD":"LDD", (op>>4)&0x1F, (op&8)?'Y':'Z', q); return b;
    }
    if ((op & 0xD200) == 0x8200) {
        int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        snprintf(b, sizeof b, "%s %c+%d, R%u", "STD", (op&8)?'Y':'Z', q, (op>>4)&0x1F); return b;
    }
    if ((op & 0xFF0F) == 0x9408 || (op & 0xFF0F) == 0x9488) {
        static const char* fn[8] = {"C","Z","N","V","S","H","T","I"};
        snprintf(b, sizeof b, "%s%s (flag %s)", (op&0xFF0F)==0x9408?"SE":"CL", fn[(op>>4)&7], fn[(op>>4)&7]); return b;
    }
    if (op == 0x9588) return "SLEEP";
    if (op == 0x95A8) return "WDR";
    if (op == 0x9698) return "BREAK";
    if ((op & 0xFE08) == 0xFC00) { snprintf(b, sizeof b, "SBRC R%u, %u", (op>>4)&0x1F, op&7); return b; }
    if ((op & 0xFE08) == 0xFE00) { snprintf(b, sizeof b, "SBRS R%u, %u", (op>>4)&0x1F, op&7); return b; }
    if ((op & 0xFE08) == 0xFA00) { snprintf(b, sizeof b, "BST  R%u, %u", (op>>4)&0x1F, op&7); return b; }
    if ((op & 0xFE08) == 0xF800) { snprintf(b, sizeof b, "BLD  R%u, %u", (op>>4)&0x1F, op&7); return b; }
    switch (op & 0xFC00) {
        case 0x0C00: snprintf(b, sizeof b, "%s  R%u", ((op>>4)&0x1F)==((op>>5)&0x10)|(op&0xF) ? "LSL" : "ADD", (op>>4)&0x1F); return b;
        case 0x1C00: snprintf(b, sizeof b, "%s  R%u", ((op>>4)&0x1F)==((op>>5)&0x10)|(op&0xF) ? "ROL" : "ADC", (op>>4)&0x1F); return b;
        case 0x1800: snprintf(b, sizeof b, "SUB  R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        case 0x0800: snprintf(b, sizeof b, "SBC  R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        case 0x1400: snprintf(b, sizeof b, "CP   R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        case 0x0400: snprintf(b, sizeof b, "CPC  R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        case 0x1000: snprintf(b, sizeof b, "CPSE R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        case 0x2000: snprintf(b, sizeof b, "%s  R%u", ((op>>4)&0x1F)==((op>>5)&0x10)|(op&0xF) ? "TST" : "AND", (op>>4)&0x1F); return b;
        case 0x2400: snprintf(b, sizeof b, "%s  R%u", ((op>>4)&0x1F)==((op>>5)&0x10)|(op&0xF) ? "CLR" : "EOR", (op>>4)&0x1F); return b;
        case 0x2800: snprintf(b, sizeof b, "OR   R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        case 0x2C00: snprintf(b, sizeof b, "MOV  R%u, R%u", (op>>4)&0x1F, ((op>>5)&0x10)|(op&0xF)); return b;
        default: break;
    }
    d = (op>>4)&0xF;
    switch (op & 0xF000) {
        case 0x3000: snprintf(b, sizeof b, "CPI  R%u, 0x%02X", d+16, (op&0xF)|((op>>4)&0xF0)); return b;
        case 0x4000: snprintf(b, sizeof b, "SBCI R%u, 0x%02X", d+16, (op&0xF)|((op>>4)&0xF0)); return b;
        case 0x5000: snprintf(b, sizeof b, "SUBI R%u, 0x%02X", d+16, (op&0xF)|((op>>4)&0xF0)); return b;
        case 0x6000: snprintf(b, sizeof b, "ORI  R%u, 0x%02X", d+16, (op&0xF)|((op>>4)&0xF0)); return b;
        case 0x7000: snprintf(b, sizeof b, "ANDI R%u, 0x%02X", d+16, (op&0xF)|((op>>4)&0xF0)); return b;
        case 0xE000: snprintf(b, sizeof b, "LDI  R%u, 0x%02X", d+16, (op&0xF)|((op>>4)&0xF0)); return b;
        case 0xC000: snprintf(b, sizeof b, "RJMP 0x%04X", g_pc + 1 + (op&0xFFF)); return b;
        case 0xD000: snprintf(b, sizeof b, "RCALL 0x%04X", g_pc + 1 + (op&0xFFF)); return b;
        case 0xB000: snprintf(b, sizeof b, "IN   R%u, 0x%02X", (op>>4)&0x1F, (op&0xF)|((op>>8)&3)<<4); return b;
        case 0xB800: snprintf(b, sizeof b, "OUT  0x%02X, R%u", (op&0xF)|((op>>8)&3)<<4, (op>>4)&0x1F); return b;
        case 0xF000: {                                            // BRBS/BRBC + aliases
            static const char* nB[8] = {"BRCS","BREQ","BRMI","BRVS","BRGE","BRHS","BRTS","BRIE"};
            static const char* nC[8] = {"BRCC","BRNE","BRPL","BRVC","BRLT","BRHC","BRTC","BRID"};
            int s = op&7, kk = (op>>3)&0x7F; if (kk >= 0x40) kk -= 0x80;
            const char* nm = (op&0xFC00)==0xF000 ? nB[s] : nC[s];
            snprintf(b, sizeof b, "%s  0x%04X", nm, g_pc + 1 + kk); return b;
        }
        default: break;
    }
    switch (op & 0xFE0F) {
        case 0x940C: return "JMP";
        case 0x940E: return "CALL";
        case 0x9400: snprintf(b, sizeof b, "COM  R%u", (op>>4)&0x1F); return b;
        case 0x9401: snprintf(b, sizeof b, "NEG  R%u", (op>>4)&0x1F); return b;
        case 0x9402: snprintf(b, sizeof b, "SWAP R%u", (op>>4)&0x1F); return b;
        case 0x9403: snprintf(b, sizeof b, "INC  R%u", (op>>4)&0x1F); return b;
        case 0x940A: snprintf(b, sizeof b, "DEC  R%u", (op>>4)&0x1F); return b;
        case 0x9405: snprintf(b, sizeof b, "ASR  R%u", (op>>4)&0x1F); return b;
        case 0x9406: snprintf(b, sizeof b, "LSR  R%u", (op>>4)&0x1F); return b;
        case 0x9407: snprintf(b, sizeof b, "ROR  R%u", (op>>4)&0x1F); return b;
        case 0x900F: snprintf(b, sizeof b, "POP  R%u", (op>>4)&0x1F); return b;
        case 0x920F: snprintf(b, sizeof b, "PUSH R%u", (op>>4)&0x1F); return b;
        case 0x9600: return "ADIW";
        case 0x9700: return "SBIW";
        case 0x9800: snprintf(b, sizeof b, "CBI  0x%02X, %u", (op>>3)&0x1F, op&7); return b;
        case 0x9A00: snprintf(b, sizeof b, "SBI  0x%02X, %u", (op>>3)&0x1F, op&7); return b;
        case 0x900C: snprintf(b, sizeof b, "LD   R%u, X", (op>>4)&0x1F); return b;
        case 0x900D: snprintf(b, sizeof b, "LD   R%u, X+", (op>>4)&0x1F); return b;
        case 0x9009: snprintf(b, sizeof b, "LD   R%u, Y+", (op>>4)&0x1F); return b;
        case 0x920C: snprintf(b, sizeof b, "ST   X, R%u", (op>>4)&0x1F); return b;
        default: break;
    }
    if (op == 0x9508) return "RET";
    if (op == 0x9518) return "RETI";
    if (op == 0x9509) return "ICALL";
    if (op == 0x9409) return "IJMP";
    snprintf(b, sizeof b, "??   0x%04X", op);
    return b;
}

// ---------------------------------------------------------------------
// TUI dashboard (README sec.11.1)
// ---------------------------------------------------------------------
static void snapPrev() {
    g_prev.clear();
    for (int i = 0; i < 32; i++) g_prev[fmt("R%d", i)] = g_dmem[i];
    for (int i = 0; i < 3; i++) {
        g_prev[fmt("DDR%s", g_pname[i])] = g_dmem[0x20 + g_pio[i].ddr];
        g_prev[fmt("PORT%s", g_pname[i])] = g_dmem[0x20 + g_pio[i].port];
    }
    g_prevValid = true;
}

static void renderDash() {
    if (g_quiet) return;
    uint16_t sp = g_dmem[0x5D] | (g_dmem[0x5E] << 8);
    double tu = (double)g_cycles * 1e6 / (double)g_fcpu;
    const char* st = g_running ? "RUNNING" : (g_loaded ? "PAUSED" : "READY");
    wr("================================================================================", DCYAN);
    wr(fmt(" [CHIP: %s]  [F_CPU: %.1f MHz]  [STATUS: %s]", g_chip.c_str(), g_fcpu/1e6, st), CYAN);
    wr(fmt(" [PC: 0x%04X]  [SP: 0x%04X]  [CYCLES: %llu]  [TIME: %.2f us]", g_pc, sp,
          (unsigned long long)g_cycles, tu), CYAN);
    wr("--------------------------------------------------------------------------------", DCYAN);
    { std::string s = " SREG:";
      const char* nm[8] = {"I","T","H","S","V","N","Z","C"};
      int vb[8] = {g_f.I,g_f.T,g_f.H,g_f.S,g_f.V,g_f.N,g_f.Z,g_f.C};
      for (int i = 0; i < 8; i++) {
          wr(fmt(" %s:%d", nm[i], vb[i]), vb[i]?GREEN:DGRAY, false);
      }
      wr(fmt("  (0x%02X)", (g_f.I<<7)|(g_f.T<<6)|(g_f.H<<5)|(g_f.S<<4)|(g_f.V<<3)|(g_f.N<<2)|(g_f.Z<<1)|g_f.C));
    }
    wr("--------------------------------------------------------------------------------", DCYAN);
    if (g_loaded) {
        int pb = g_pc * 2;
        for (int off = -2; off <= 2; off += 2) {
            int w = g_pc + off;
            if (w < 0 || w*2 >= (int)FLASH_END) continue;
            std::string txt = decodeOp(flashWord(w));
            if (off == 0) { wr(fmt(" -> [0x%04X] 0x%04X  %s", w, flashWord(g_pc), txt.c_str()), YELLOW); }
            else          { wr(fmt("    [0x%04X] 0x%04X  %s", w, flashWord(w), txt.c_str())); }
        }
        (void)pb;
    }
    wr("--------------------------------------------------------------------------------", DCYAN);
    wr(" [REGISTERS R0-R31]  (* = changed on last step)", CYAN);
    for (int row = 0; row < 8; row++) {
        std::string line;
        bool anyCh = false;
        for (int col = 0; col < 4; col++) {
            int r = row + col*8;
            std::string mark;
            std::string key = fmt("R%d", r);
            if (g_prevValid && g_prev.count(key) && g_prev[key] != g_dmem[r]) { mark = "*"; anyCh = true; }
            line += fmt("%sR%02d: 0x%02X (%3d)     ", mark.c_str(), r, g_dmem[r], (int8_t)g_dmem[r]);
        }
        wr(line, anyCh?RED:GRAY);
    }
    wr("--------------------------------------------------------------------------------", DCYAN);
    for (int i = 0; i < 3; i++) {
        uint8_t ddr = g_dmem[0x20+g_pio[i].ddr], prt = g_dmem[0x20+g_pio[i].port];
        uint8_t pin = readDmem(0x20+g_pio[i].pin);
        char ddrb[16], prtb[16], pinb[16];
        for (int b = 0; b < 8; b++) { ddrb[7-b] = ((ddr>>b)&1)+'0'; prtb[7-b] = ((prt>>b)&1)+'0'; pinb[7-b] = ((pin>>b)&1)+'0'; }
        ddrb[8] = prtb[8] = pinb[8] = 0;
        bool ch = g_prevValid && g_prev.count(fmt("DDR%s", g_pname[i])) && g_prev[fmt("DDR%s", g_pname[i])] != ddr;
        wr(fmt(" [PORT%s] DDR%s: 0b%s (0x%02X) | PORT%s: 0b%s (0x%02X) | PIN%s: 0b%s (0x%02X)",
               g_pname[i], g_pname[i], ddrb, ddr, g_pname[i], prtb, prt, g_pname[i], pinb, pin),
            ch ? RED : GRAY);
    }
    if (!g_watch.empty()) {
        wr("--------------------------------------------------------------------------------", DCYAN);
        wr(" [WATCH]", CYAN);
        for (auto& w : g_watch) {
            int v = readDmem(w.addr);
            wr(fmt(" [%s] @ 0x%04X = %d (0x%02X)", w.name.c_str(), w.addr, v, v));
        }
    }
    // stopwatch
    wr("================================================================================", DCYAN);
}

// ---------------------------------------------------------------------
// Self-test (mirrors the PS version)
// ---------------------------------------------------------------------
static int runSelfTest() {
    int failCount = 0, passCount = 0;
    auto tc = [&](const char* nm, int got, int want) {
        if (got == want) { passCount++; wr(fmt("  [PASS] %s", nm), GREEN); }
        else { failCount++; wr(fmt("  [FAIL] %s: got %d (0x%X), want %d (0x%X)", nm, got, got, want, want), RED); }
    };
    wr("-- Emulator self-test (LDI/ADD/SUBI/SBIW + flags)", CYAN);
    static const uint8_t prog[] = {0x05,0xE5, 0x13,0xE0, 0x01,0x0F, 0x00,0x56, 0x82,0xE3, 0xC2,0x97};
    memset(g_flash, 0xFF, sizeof(g_flash));
    memcpy(g_flash, prog, sizeof(prog));
    std::fill(g_dmem.begin(), g_dmem.end(), 0);
    g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END>>8)&0xFF;
    g_loaded = true; g_pc = 0; g_cycles = 0; g_f = {};

    uint16_t op; int cy;
    execOne(op, cy); tc("LDI R16,0x55", g_dmem[16], 0x55);
    execOne(op, cy); tc("LDI R17,0x03", g_dmem[17], 0x03);
    execOne(op, cy); tc("ADD R16,R17 = 0x58", g_dmem[16], 0x58);
                       tc("ADD C=0", g_f.C, 0);
    execOne(op, cy); tc("SUBI R16 -> 0xF8 (-8)", g_dmem[16], 0xF8);
                       tc("SUBI C=1", g_f.C, 1);
                       tc("SUBI N=1", g_f.N, 1);
    execOne(op, cy); tc("LDI R24,50 (0x32)", g_dmem[24], 0x32);
    execOne(op, cy); tc("SBIW result = 0", g_dmem[24] | (g_dmem[25]<<8), 0);
                       tc("SBIW Z flag", g_f.Z, 1);

    if (!failCount) wr(fmt(" Self-test: ALL PASSED (%d checks)", passCount), GREEN);
    else wr(fmt(" Self-test: %d FAILED", failCount), RED);
    return failCount;
}

// ---------------------------------------------------------------------
// Self-test 2: new instructions (MOVW/MUL/LDS/STS/LDD/STD/BST/BLD/SBRS/SEC)
// ---------------------------------------------------------------------
static int runSelfTest2() {
    int failCount = 0, passCount = 0;
    auto tc = [&](const char* nm, int got, int want) {
        if (got == want) { passCount++; wr(fmt("  [PASS] %s", nm), GREEN); }
        else { failCount++; wr(fmt("  [FAIL] %s: got 0x%02X, want 0x%02X", nm, got, want), RED); }
    };
    wr("-- Emulator self-test 2 (MOVW/MUL/LDS/STS/LDD/STD/BST/BLD/SBRS/SEC)", CYAN);
    static const uint8_t prog[] = {
        0x0F,0xEF,   // LDI R16,0xFF
        0x08,0x94,   // SEC  (BSET 0)
        0x03,0xFB,   // BST R16,3   -> T=1
        0x10,0xE0,   // LDI R17,0x00
        0x15,0xF9,   // BLD R17,5   -> R17=0x20
        0x15,0xFF,   // SBRS R17,5  -> skip next (bit5==1)
        0x77,0xE2,   // LDI R18,0x77   (must be skipped)
        0x98,0x01,   // MOVW R18:19 <- R16:17
        0x8F,0xE0,   // LDI R24,15
        0x80,0x9F,   // MUL R24,R16 -> R1:R0 = 15*255 = 0x0EF1
        0x00,0x93,   // STS 0x61, R0
        0x61,0x00,
        0x40,0x91,   // LDS R20, 0x61 -> 0xF1
        0x61,0x00,
        0xE0,0xE6,   // LDI R30,0x60 (Z low)
        0xF0,0xE0,   // LDI R31,0    (Z high)
        0x12,0x82,   // STD Z+2, R1  -> [0x62] = 0x0E
        0x51,0x81,   // LDD R21, Z+1 <- [0x61] = 0xF1
        0x62,0x81 }; // LDD R22, Z+2 <- [0x62] = 0x0E
    memset(g_flash, 0xFF, sizeof(g_flash));
    memcpy(g_flash, prog, sizeof(prog));
    std::fill(g_dmem.begin(), g_dmem.end(), 0);
    g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END>>8)&0xFF;
    g_loaded = true; g_pc = 0; g_cycles = 0; g_f = {};

    uint16_t op; int cy;
    for (int i = 0; i < 6; i++) { execOne(op, cy); wr(fmt("  dbg pc=0x%04X op=0x%04X %s", g_pc, op, decodeOp(op).c_str()), CYAN); }
    tc("SEC (BSET0) C=1", g_f.C, 1);
    tc("BST R16,3 -> T=1", g_f.T, 1);
    tc("BLD R17,5 -> 0x20", g_dmem[17], 0x20);
    tc("SBRS skipped LDI (R18=0)", g_dmem[18], 0);
    execOne(op, cy);                                     // MOVW
    wr(fmt("  dbg pc=0x%04X op=0x%04X R0=0x%02X R1=0x%02X [0x60]=0x%02X [0x61]=0x%02X [0x62]=0x%02X", g_pc, op, g_dmem[0], g_dmem[1], g_dmem[0x60], g_dmem[0x61], g_dmem[0x62]), CYAN);
    tc("MOVW R18 = 0xFF", g_dmem[18], 0xFF);
    tc("MOVW R19 = 0x20", g_dmem[19], 0x20);
    execOne(op, cy); execOne(op, cy);                    // LDI R24, MUL
    tc("MUL R0 = 0xF1", g_dmem[0], 0xF1);
    tc("MUL R1 = 0x0E", g_dmem[1], 0x0E);
    execOne(op, cy);                                     // STS 0x61, R0
    wr(fmt("  dbg STS: pc=0x%04X op=0x%04X R0=0x%02X [0x61]=0x%02X", g_pc, op, g_dmem[0], g_dmem[0x61]), CYAN);
    execOne(op, cy);                                     // LDS R20
    wr(fmt("  dbg LDS: pc=0x%04X op=0x%04X R20=0x%02X kk-PC-th word test", g_pc, op, g_dmem[20]), CYAN);
    tc("LDS/STS R20 = 0xF1", g_dmem[20], 0xF1);
    execOne(op, cy); execOne(op, cy);                    // Z = 0x60 (LDI R30/R31)
    execOne(op, cy);                                     // STD Z+2, R1
    tc("STD [0x62] = 0x0E", g_dmem[0x62], 0x0E);
    execOne(op, cy);                                     // LDD R21, Z+1
    tc("LDD R21 = 0xF1", g_dmem[21], 0xF1);
    execOne(op, cy);                                     // LDD R22, Z+2
    tc("LDD R22 = 0x0E", g_dmem[22], 0x0E);

    if (!failCount) wr(fmt(" Self-test 2: ALL PASSED (%d checks)", passCount), GREEN);
    else wr(fmt(" Self-test 2: %d FAILED", failCount), RED);
    return failCount;
}

// ---------------------------------------------------------------------
// Command dispatcher (README sec.12)
// ---------------------------------------------------------------------
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> v; std::istringstream iss(s); std::string t;
    while (iss >> t) v.push_back(t);
    return v;
}
static int parseNum(const std::string& s, bool& ok) {
    ok = true; int base = 10;
    std::string t = s; bool neg = false;
    if (!t.empty() && (t[0]=='-' || t[0]=='+')) { neg = t[0]=='-'; t = t.substr(1); }
    if (t.size() > 2 && (t[0]=='0') && (t[1]=='x' || t[1]=='X')) { base = 16; t = t.substr(2); }
    char* endp = nullptr;
    long v = strtol(t.c_str(), &endp, base);
    if (endp == t.c_str()) { ok = false; return 0; }
    return neg ? -(int)v : (int)v;
}

static bool dispatch(const std::string& line) {
    auto tk = splitWs(line);
    if (tk.empty()) return true;
    std::string cmd = lower(tk[0]);
    auto a = [&](int i) -> std::string { return tk.size() > (size_t)i ? tk[i] : std::string(); };

    if (cmd == "emu") {
        std::string s1 = lower(a(1));
        if (s1 == "start" || s1 == "load" || s1 == "reset") {
            std::string err;
            if (a(2) == "reset" || s1 == "reset") {
                g_pc = 0; g_cycles = 0;
                std::fill(g_dmem.begin(), g_dmem.end(), 0);
                g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END>>8)&0xFF;
                snapPrev(); renderDash(); wr("Reset done.", GREEN); return true;
            }
            if (loadElfSys(g_elf, err)) { snapPrev(); wr("Loaded: " + g_elf, GREEN); renderDash(); }
            else wr("Load failed: " + err, RED);
        } else if (s1 == "stop") { g_running = false; }
        return true;
    }
    if (cmd == "load" || cmd == "emu.start") {
        std::string err;
        if (loadElfSys(g_elf, err)) { snapPrev(); wr("Loaded: " + g_elf, GREEN); renderDash(); }
        else wr("Load failed: " + err, RED);
        return true;
    }
    if (cmd == "reset" || cmd == "rst") {
        g_pc = 0; g_cycles = 0; g_running = false;
        std::fill(g_dmem.begin(), g_dmem.end(), 0);
        for (int i = 0; i < 3; i++) { g_ext[i].mask = 0; g_ext[i].level = 0; }
        g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END>>8)&0xFF;
        snapPrev(); renderDash(); wr("Reset done.", GREEN); return true;
    }
    if (cmd == "stepi" || cmd == "si" || cmd == "step" || cmd == "s") {
        int n = 1;
        if (!a(1).empty()) { bool ok; n = parseNum(a(1), ok); if (!ok) n = 1; }
        snapPrev();
        snapPrev();
        for (int i = 0; i < n; i++) {
            uint16_t op; int cy;
            if (!execOne(op, cy)) break;
            renderDash();
            Sleep(60);
        }
        return true;
    }
    if (cmd == "run" || cmd == "r") {
        if (!g_loaded) { wr("! No program loaded. Use: emu start", RED); return true; }
        uint64_t maxC = 2000000;
        if (!a(1).empty()) { bool ok; long v = parseNum(a(1), ok); if (ok) maxC = (uint64_t)v; }
        g_running = true;
        uint64_t cnt = 0;
        while (g_running && cnt < maxC) {
            auto bi = g_bp.find(g_pc);
            if (bi != g_bp.end()) {
                wr(fmt("-- Breakpoint #%d hit at PC=0x%04X (cycle %llu)", bi->second, g_pc, (unsigned long long)g_cycles), RED);
                g_running = false; break;
            }
            uint16_t op; int cy;
            if (!execOne(op, cy)) break;
            cnt++;
        }
        g_running = false;
        renderDash();
        if (cnt >= maxC) wr(fmt("-- Run stopped at max-cycles limit (%llu).", (unsigned long long)maxC), YELLOW);
        return true;
    }
    if (cmd == "reg" || cmd == "rg") {
        std::string s1 = lower(a(1));
        if (s1 == "all" || s1.empty()) {
            for (int i = 0; i < 32; i++) wr(fmt(" R%02d = 0x%02X (%d)", i, g_dmem[i], g_dmem[i]));
        } else if (s1[0] == 'r' || s1[0] == 'R') {
            bool ok; int idx = parseNum(s1.substr(1), ok);
            if (ok && idx >= 0 && idx < 32) {
                if (!a(2).empty()) { bool ok2; int v = parseNum(a(2), ok2); if (ok2) g_dmem[idx] = (uint8_t)(v & 0xFF); }
                int v = g_dmem[idx];
                wr(fmt(" R%02d = 0x%02X unsigned %d signed %d", idx, v, v, (int8_t)v), CYAN);
            } else wr("Register index out of range (R0..R31)", RED);
        } else if (s1 == "sreg") {
            wr(fmt(" SREG I=%d T=%d H=%d S=%d V=%d N=%d Z=%d C=%d", g_f.I,g_f.T,g_f.H,g_f.S,g_f.V,g_f.N,g_f.Z,g_f.C), CYAN);
        } else wr("Usage: reg R<index> [= val] or: reg all", RED);
        return true;
    }
    if (cmd == "port" || cmd == "pt") {
        std::string p = upper(a(1));
        int pi = (p == "B") ? 0 : (p == "C") ? 1 : (p == "D") ? 2 : -1;
        if (pi < 0) { wr("Usage: port <B|C|D>", RED); return true; }
        if (g_quiet) return true;
        uint8_t ddr = g_dmem[0x20+g_pio[pi].ddr], prt = g_dmem[0x20+g_pio[pi].port];
        uint8_t pin = readDmem(0x20+g_pio[pi].pin);
        for (int b = 0; b < 8; b++) {
            const char* dir = ((ddr>>b)&1) ? "OUT" : "IN ";
            const char* lvl = ((pin>>b)&1) ? "HIGH" : "LOW ";
            const char* pull = ((prt>>b)&1) ? "PullUp" : "Hi-Z ";
            wr(fmt("  %s%d: %s %s %s%s", p.c_str(), b, dir, lvl, pull,
                   ((g_ext[pi].mask>>b)&1) ? " [forced]" : ""), CYAN);
        }
        return true;
    }
    if (cmd == "pin" || cmd == "pn") {
        std::string pn = upper(a(1)), val = lower(a(2));
        if (pn.size() >= 2 && (pn[0] == 'P')) {
            int pi = (pn[1] == 'B') ? 0 : (pn[1] == 'C') ? 1 : (pn[1] == 'D') ? 2 : -1;
            if (pi < 0) { wr("Unknown port (B/C/D on atmega8)", RED); return true; }
            bool isBit = (pn.size() >= 3) && isdigit((unsigned char)pn[2]) && pn.size() == 3;
            uint8_t cur = g_ext[pi].mask ? g_ext[pi].level : 0;
            if (val == "toggle") {
                if (isBit) { int b = pn[2]-'0'; cur ^= (1<<b); g_ext[pi].mask |= (1<<b); }
                else cur = ~cur;
            } else {
                bool ok; int v = parseNum(val, ok);
                if (!ok) { wr("Usage: pin <P#|P#bit> <0..255|1|0|toggle>", RED); return true; }
                if (isBit) { int b = pn[2]-'0';
                    if (v) cur |= (1<<b); else cur &= ~(1<<b);
                    g_ext[pi].mask |= (1<<b);
                } else { cur = (uint8_t)v; g_ext[pi].mask = 0xFF; }
            }
            g_ext[pi].level = cur;
            wr(fmt("Stimulus applied to %s: mask 0x%02X level 0x%02X", pn.c_str(), g_ext[pi].mask, g_ext[pi].level), GREEN);
            renderDash();
        } else wr("Usage: pin <P#|P#bit> <0|1|toggle|byte>", RED);
        return true;
    }
    if (cmd == "mem" || cmd == "m") {
        std::string region = lower(a(1));
        bool ok; uint32_t addr = a(2).empty() ? 0 : parseNum(a(2), ok);
        int len = 16; if (!a(3).empty()) { bool ok3; len = parseNum(a(3), ok3); }
        for (int i = 0; i < len; i += 8, addr += 8) {
            char hx[32]; int w = 0;
            for (int j = 0; j < 8 && (i+j) < len; j++) {
                int aa = (int)addr + j, v;
                if (region == "flash") v = (aa < (int)FLASH_END) ? g_flash[aa] : 0xFF;
                else v = aa < (int)DMEM_SIZE ? readDmem(aa) : 0;
                w += snprintf(hx + w, sizeof(hx)-w, "%02X ", v);
            }
            wr(fmt(" 0x%04X: %s", addr, hx));
        }
        return true;
    }
    if (cmd == "w" || cmd == "watch") {
        if (!a(1).empty()) { bool ok; int v = parseNum(a(1), ok);
            if (ok) { g_watch.push_back({a(1), (uint16_t)v}); wr(fmt("Watch added: addr 0x%04X", v), GREEN); } }
        return true;
    }
    if (cmd == "bp") {
        std::string s1 = lower(a(1));
        if (s1 == "add" || s1 == "ba") {
            uint32_t addr = 0; bool ok = false;
            uint16_t pc = 0xFFFF;
            if (!a(2).empty() && isdigit((unsigned char)a(2)[0])) {
                bool o2; addr = parseNum(a(2), o2); ok = o2; pc = addr;
                // addresses may be byte addresses: treat < FLASH_END as word index directly
            } else if (!a(2).empty()) {
                uint32_t sym = 0;
                if (findSymbol(a(2), sym)) { ok = true; pc = (uint16_t)(sym / 2); }
                else wr(fmt("bp add: cannot resolve symbol '%s'", a(2).c_str()), RED);
            }
            if (ok && pc != 0xFFFFu) {
                g_bp[pc] = (int)g_bpSerial++; wr(fmt("Breakpoint #%d at PC=0x%04X", g_bpSerial-1, pc), GREEN);
            }
        } else if (s1 == "list" || s1 == "bl") {
            for (auto& bp : g_bp) wr(fmt(" #%d: PC=0x%04X", bp.second, bp.first), CYAN);
        } else if (s1 == "del" || s1 == "bd") {
            std::string s2 = lower(a(2));
            if (s2 == "all") g_bp.clear();
            else { bool ok; int id = parseNum(s2, ok); if (ok) for (auto it = g_bp.begin(); it != g_bp.end(); ++it) if (it->second == id) { g_bp.erase(it); break; } }
        }
        return true;
    }
    if (cmd == "ba") return dispatch("bp add " + a(1));
    if (cmd == "bl") return dispatch("bp list");
    if (cmd == "bd") return dispatch("bp del " + a(1));
    if (cmd == "tr" || cmd == "timer") {
        g_cycles = 0; wr("Cycle counter / stopwatch reset.", GREEN); return true;
    }
    if (cmd == "size" || cmd == "sz") {
        int used = 0;
        for (int i = (int)FLASH_END - 2; i >= 0; i -= 2)
            if (g_flash[i] != 0xFF || g_flash[i+1] != 0xFF) { used = i + 2; break; }
        wr(fmt("Program: %d / %d bytes (%.1f%%) | SRAM: R0-R31 + I/O reserved", used, FLASH_END,
               100.0 * used / FLASH_END), CYAN);
        return true;
    }
    if (cmd == "da" || cmd == "disasm") {
        for (int i = -2; i <= 4; i += 2) {
            int w = g_pc + i;
            if (w < 0 || w*2 >= (int)FLASH_END) continue;
            char m[4] = "  ";
            if (i == 0) { m[0] = '-'; m[1] = '>'; }
            if (g_quiet) continue;
            wr(fmt(" %s [0x%04X] 0x%04X  %s", m, w, flashWord(w), decodeOp(flashWord(w)).c_str()), i==0?YELLOW:GRAY);
        }
        return true;
    }
    if (cmd == "auto" || cmd == "au") {
        int n = a(1).empty() ? 2 : atoi(a(1).c_str());
        int dms = a(2).empty() ? 500 : atoi(a(2).c_str());
        snapPrev();
        for (int i = 0; i < n; i++) {
            uint16_t op; int cy;
            if (!execOne(op, cy)) break;
            renderDash();
            Sleep(dms);
        }
        return true;
    }
    if (cmd == "quit" || cmd == "exit" || cmd == "q") return false;
    if (cmd == "selftest") { runSelfTest(); return true; }
    if (cmd == "selftest2" || cmd == "st2") { runSelfTest2(); return true; }
    if (cmd == "quiet") { g_quiet = true; return true; }
    if (cmd == "help" || cmd == "?") {
        wr("Commands (from README sec.12):", CYAN);
        wr("  emu start|load|reset    build info shown as 'emu' section");
        wr("  step|stepi [n]          auto/until/run [maxcyc]");
        wr("  reg [R#|sreg|all] [=val] port <B|C|D>   pin <P#|P#bit> <v|toggle>");
        wr("  mem <sram|flash> <addr> [len]    ba <addr|symbol>   bl  bd <n|all>");
        wr("  tr (timer reset)  size  da (disasm)  w <addr> (watch)  silent ");
        wr("  silent  quiet  help  selftest  selftest2  quit");
        return true;
    }
    wr("Unknown command: " + cmd + ". Type 'help'.", RED);
    return true;
}
static std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
static std::string exeDir() {
    char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p; size_t sl = s.find_last_of("\\/"); if (sl != std::string::npos) s = s.substr(0, sl);
    return s;
}
int main(int argc, char** argv) {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);
    std::string runScript;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-f" || a == "--file" || a == "-s" || a == "--script") { if (++i < argc) runScript = argv[i]; }
        else if (a == "-e" || a == "--elf") { if (++i < argc) g_elf = argv[i]; }
        else if (a == "-q" || a == "--quiet") g_quiet = true;
        else if (a == "--root") { if (++i < argc) g_root = argv[i]; }
    }
    // root: exe is in <root>/build
    if (g_root.empty()) { std::string d = exeDir(); size_t sl = d.find_last_of("\\/"); g_root = (sl != std::string::npos) ? d.substr(0, sl) : "."; }
    if (g_elf.empty()) g_elf = g_root + "/main.elf";

    loadEnv();

    wr("================================================================================");
    wr(fmt("  AVR CLI Studio (C++)  |  Core: %s  |  F_CPU = %u Hz", g_chip.c_str(), g_fcpu), CYAN);
    wr("  Type 'help' for commands.                                      (emu.cpp v0.2)", DCYAN);
    wr("================================================================================");

    if (!runScript.empty()) {
        std::ifstream f(runScript);
        if (!f) { wr("Cannot open script: " + runScript, RED); return 1; }
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            if (!dispatch(line)) break;
        }
        return 0;
    }

    std::string line;
    while (true) {
        wr("AVR-CLI> ", YELLOW, false);
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (!dispatch(line)) break;
    }
    return 0;
}
