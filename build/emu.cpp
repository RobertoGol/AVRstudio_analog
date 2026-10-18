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
static uint32_t FLASH_END = 0x2000;   // last FLASH byte (set from profile; ATmega8 = 0x2000)
static uint32_t DMEM_SIZE = 0x460;    // RAM_END+1
static uint16_t RAM_END   = 0x045F;

static std::vector<uint8_t> g_flash(FLASH_END, 0xFF);
static std::vector<uint8_t> g_dmem(DMEM_SIZE, 0);
static std::vector<uint8_t> g_eeprom;     // real EEPROM backing (init 0xFF, persists across reset like hardware)
static int g_eeCR = 0x3C, g_eeDR = 0x3D, g_eeARL = 0x3E, g_eeARH = 0x3F;   // SRAM-адреса EECR/EEDR/EEAR (из профиля)

// Terminal I/O (как в AVR Studio): TX-лог (записи в UDR) + очередь RX
static std::string g_tioTx;
static std::vector<uint8_t> g_tioRx;
static int g_udr = -1, g_ucsrA = -1, g_ucsrB = -1;  // SRAM-адреса UDR/UCSRA/UCSRB (из профиля)
static uint16_t g_pc = 0;           // in WORDS
static uint64_t g_cycles = 0;
static bool     g_loaded = false;
static bool     g_running = false;
static bool     g_quiet   = false;

// SREG flags
static struct { int I,T,H,S,V,N,Z,C; } g_f = {};

// GPIO external stimulus: per port forced mask + forced value (runtime, from profile)
struct ExtStim { uint8_t mask, level; };
static std::vector<ExtStim> g_ext = { {0,0},{0,0},{0,0} };
// port maps: IO addresses {PIN, DDR, PORT} + letter names
struct Pio { int pin, ddr, port; };
static std::vector<Pio> g_pio = { {0x16,0x17,0x18},{0x13,0x14,0x15},{0x10,0x11,0x12} };
static std::vector<std::string> g_pname = { "B","C","D" };
static int portIndex(char c) {
    for (int i = 0; i < (int)g_pio.size(); i++)
        if (toupper((unsigned char)g_pname[i][0]) == toupper((unsigned char)c)) return i;
    return -1;
}

// config
static std::string g_chip = "ATmega8A";
static std::string g_mcu  = "atmega8";
static uint32_t    g_fcpu = 8000000;

// breakpoints: index by PC (word). value = id
static std::map<uint16_t,int> g_bp;
static std::map<uint16_t,int>::size_type g_bpSerial = 1;
// internal temp breakpoints (next/until); cleared after each command
static std::map<uint16_t,int> g_bpTemp;

// call-family detection (for step-over semantics)
static bool isCallOp(uint16_t op) {
    if ((op & 0xF000) == 0xD000) return true;          // RCALL
    if ((op & 0xFE0F) == 0x940E) return true;          // JMP/CALL (2-word)
    if (op == 0x9509 || op == 0x9519) return true;     // ICALL/EICALL
    return false;
}
static int isCall2Word(uint16_t op) { return ((op & 0xFE0F) == 0x940E) ? 2 : 1; }

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

static uint64_t parseAddr(const std::string& sv) {
    std::string v = sv;
    size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
    v.erase(0, v.find_first_not_of(" \t")); v.erase(v.find_last_not_of(" \t\r")+1);
    if (v.empty()) return 0;
    return strtoull(v.c_str(), nullptr, 0);   // handles 0xNN and decimal
}
static const char* faPres(int n) {      // timer prescaler label
    switch (n) {
        case 1: return "1x"; case 8: return "8x"; case 32: return "32x"; case 64: return "64x";
        case 128: return "128x"; case 256: return "256x"; case 1024: return "1024x";
        default: return "?";
    }
}

// SPM state (used by loadEnv below; implementation near execOne)
static uint16_t g_spmPageSize = 64;                 // SPM_PAGESIZE (bytes), from the profile
static uint8_t  g_sig[3] = { 0x1E, 0x93, 0x07 };    // device signature (profile)
static std::vector<uint8_t> g_pagebuf;              // page buffer (g_spmPageSize bytes)

static void spmInit() {
    if (g_spmPageSize < 2) g_spmPageSize = 64;
    if (g_spmPageSize & (g_spmPageSize - 1)) {      // force power of two
        uint16_t p = 2; while (p * 2 <= g_spmPageSize) p *= 2; g_spmPageSize = p;
    }
    g_pagebuf.assign(g_spmPageSize, 0xFF);
}

// ---------------------------------------------------------------------
// Interrupt dispatcher state (README sec.5.2)
// Vectors come from the profile [Interrupts]; registers from [RegisterMap].
// ---------------------------------------------------------------------
static std::map<std::string,int> g_isrv;    // lowercase vector name -> vector index (PC word == index)
static std::map<std::string,int> g_regmap;  // lowercase register name -> SRAM address (IO+0x20 / MEM direct)
struct TimerDef {                                     // resolved per profile at load
    int have = 0;
    int tcntL = 0, tcntH = -1;                        // -1 = 8-bit
    int tccr = 0, tifr = 0, timsk = 0;                // SRAM addresses
    int tovBit = 0, toieBit = 0;                      // flag/enable bit positions
};
static TimerDef  g_ts[3];                   // T0, T1, T2
static uint64_t  g_tAcc[3] = { 0,0,0 };     // sub-prescaler cycle accumulators
static int       g_int0PinIO = 0x10, g_int0Bit = 2;   // INT0 pin (default PD2)
static int       g_int1PinIO = 0x10, g_int1Bit = 3;   // INT1 pin (default PD3)
static bool      g_edgeInit = false;        // no spurious edge on first tick after load/rst
static void resetInts() {
    g_tAcc[0] = g_tAcc[1] = g_tAcc[2] = 0;
    g_edgeInit = false;
}
static uint8_t g_edgeSt[2] = { 0, 0 };      // previous logical level of INT0/INT1 pins (edge detector)

static void loadEnv() {
    auto cfg = readPairs(g_root + "/echo.ctos");
    auto it  = [&cfg](const char* k){ auto i = cfg.find(k); return i!=cfg.end()? i->second : std::string(); };
    if (!it("mcu_target").empty())       g_mcu  = it("mcu_target");
    if (!it("processorprofile").empty()) g_chip = it("processorprofile");
    if (!it("f_cpu").empty())  g_fcpu = (uint32_t)atoll(it("f_cpu").c_str());

    std::string prof = g_root + "/Procs/" + g_chip + ".proc";
    auto p = readPairs(prof);
    auto pit = [&p](const char* k){ auto i = p.find(k); return i!=p.end()? i->second : std::string(); };
    if (!pit("name").empty()) g_chip = pit("name");
    // memory geometry from profile (README sec.8: [Memory] keys)
    std::string fe = pit("flashend"), re = pit("ramend");
    if (!fe.empty()) { uint64_t v = parseAddr(fe); if (v >= 256 && v <= 256*1024) { FLASH_END = (uint32_t)v; g_flash.assign(FLASH_END, 0xFF); } }
    if (!re.empty()) { uint64_t v = parseAddr(re); if (v >= 64 && v <= 64*1024) { RAM_END = (uint16_t)v; DMEM_SIZE = RAM_END + 1; g_dmem.assign(DMEM_SIZE, 0); } }
    std::string ee = pit("eepromend");
    if (!ee.empty()) { uint64_t v = parseAddr(ee); if (v >= 16 && v <= 8192) g_eeprom.assign((size_t)v + 1, 0xFF); }   // EepromEnd = SIZE-1
    std::string ps = pit("spmpagesize");
    if (!ps.empty()) { uint64_t v = parseAddr(ps); if (v >= 2 && v <= 4096) g_spmPageSize = (uint16_t)v; }
    spmInit();
    std::string sg = pit("signature");
    if (!sg.empty()) {
        // "0x1E 0x93 0x07"
        unsigned b0=0,b1=0,b2=0;
        if (sscanf(sg.c_str(), "%x %x %x", &b0,&b1,&b2) == 3 && b1 < 256 && b2 < 256) {
            g_sig[0]=(uint8_t)b0; g_sig[1]=(uint8_t)b1; g_sig[2]=(uint8_t)b2;
        }
    }
    // [Ports]: single-letter keys "B = PIN_IO DDR_IO PORT_IO"
    std::vector<Pio> np; std::vector<std::string> nn;
    for (auto& kv : p) {                                   // map sorted: A,B,C,D order
        const std::string& k = kv.first; const std::string& v = kv.second;
        if (k.size() != 1 || !isalpha((unsigned char)k[0])) continue;
        if (v.find("0x") == std::string::npos && v.find("0X") == std::string::npos) continue;
        // strip inline comment, collect 3 hex tokens (PIN DDR PORT)
        std::string vl = v; size_t hsh = vl.find('#');
        if (hsh != std::string::npos) vl = vl.substr(0, hsh);
        unsigned pinv, ddrv, portv;
        if (sscanf(vl.c_str(), "%x %x %x", &pinv, &ddrv, &portv) == 3) {
            nn.push_back(std::string(1, (char)toupper((unsigned char)k[0])));
            np.push_back({ (int)pinv, (int)ddrv, (int)portv });
        }
    }
    if (!np.empty()) { g_pio = np; g_pname = nn; g_ext.clear(); g_ext.resize(np.size()); }

    // [RegisterMap] lines "NAME|TYPE|ADDR": NAME(lower) -> SRAM address.
    // IO8 addresses get +0x20 (IO space -> SRAM view); MEM8 stay as-is.
    g_regmap.clear();
    {
        std::ifstream f(prof);
        std::string ln; bool inRM = false;
        while (std::getline(f, ln)) {
            size_t s = ln.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            ln = ln.substr(s);
            if (ln[0] == '[') {
                size_t e = ln.find(']');
                std::string sec = (e != std::string::npos) ? ln.substr(1, e-1) : ln.substr(1);
                inRM = (lower(sec) == "registermap");
                continue;
            }
            if (!inRM || ln[0] == '#') continue;
            size_t p1 = ln.find('|'); if (p1 == std::string::npos) continue;
            size_t p2 = ln.find('|', p1+1); if (p2 == std::string::npos) continue;
            std::string nm  = lower(ln.substr(0, p1));
            std::string tp  = lower(ln.substr(p1+1, p2-p1-1));
            int addr = (int)parseAddr(ln.substr(p2+1));
            if (nm.empty() || addr < 0 || addr > 0x2000) continue;
            if (tp.find("io") == 0) addr += 0x20;    // IO address
            g_regmap[nm] = addr;
        }
    }

    // USART/EEPROM регистры: адреса берём из профиля (не хардкод под ATmega8)
    {
        auto rr = [&](const char* n) -> int { auto i = g_regmap.find(n); return i != g_regmap.end() ? i->second : -1; };
        g_udr   = rr("udr");   if (g_udr   < 0) g_udr   = rr("udr0");
        g_ucsrA = rr("ucsra"); if (g_ucsrA < 0) g_ucsrA = rr("ucsra0");
        g_ucsrB = rr("ucsrb"); if (g_ucsrB < 0) g_ucsrB = rr("ucsr0b");
        g_eeCR  = rr("eecr");  g_eeDR = rr("eedr");
        g_eeARL = rr("eearl"); g_eeARH = rr("eearh");
        if (g_eeARL < 0 && g_eeARH < 0) g_eeARL = rr("eear");   // однобайтный EEAR у старых/tiny чипов
        if (g_eeCR < 0 || g_eeDR < 0 || g_eeARL < 0) { g_eeCR = 0x3C; g_eeDR = 0x3D; g_eeARL = 0x3E; g_eeARH = 0x3F; }  // мега8-легаси
    }

    // [Interrupts] rows "INDEX = NAME_vectors" -> vector table (PC word == index)
    g_isrv.clear();
    for (auto& kv : p) {
        std::string v = lower(kv.second); size_t h = v.find('#');
        if (h != std::string::npos) v = v.substr(0, h); v.erase(v.find_last_not_of(" \t")+1);
        if (v.size() <= 5 || v.substr(v.size()-5) != "_vect") continue;
        unsigned idx = 0;
        if (sscanf(kv.first.c_str(), "%u", &idx) != 1 || idx == 0) continue;
        g_isrv[v] = (int)idx;
    }

    // External interrupt pins: profile keys "Int0Pin = D2" (port letter + bit)
    {
        auto rp = [&](const char* key, int& pinIO, int& bit, int defPin, int defBit) {
            pinIO = defPin; bit = defBit;
            auto i = p.find(key); if (i == p.end()) return;
            std::string s = i->second; size_t h = s.find('#');
            if (h != std::string::npos) s = s.substr(0, h);
            s.erase(0, s.find_first_not_of(" \t")); s.erase(s.find_last_not_of(" \t\r")+1);
            if (s.size() < 2 || !isdigit((unsigned char)s[1]) || s[1] > '7') return;
            int pi = portIndex(toupper((unsigned char)s[0]));
            if (pi < 0) return;
            pinIO = g_pio[pi].pin;
            bit = s[1] - '0';
        };
        rp("int0pin", g_int0PinIO, g_int0Bit, 0x10, 2);
        rp("int1pin", g_int1PinIO, g_int1Bit, 0x10, 3);
    }

    // Timer definitions (per chip family; verified against toolchain headers):
    //  unified (m8/m16/m32): TIFR/TIMSK, TOV0=bit0, TOV1=bit2, TOV2=bit6
    //  split (m88/m168/m328): TIFR0/1/2 + TIMSK0/1/2, all bits=0 (TOV2=1 on tn13)
    //  tn13: single TIFR0/TIMSK0, old datasheet bit1
    auto ra = [&](const char* n) -> int { auto i = g_regmap.find(n); return i != g_regmap.end() ? i->second : -1; };
    for (int t = 0; t < 3; t++) g_ts[t] = TimerDef();
    bool split = ra("tifr0") >= 0 && ra("tifr1") >= 0 && ra("tifr") < 0;
    bool tn13  = ra("tifr0") >= 0 && ra("tifr1") <  0 && ra("tifr") < 0;
    if (ra("tcnt0") >= 0) {
        int tccr = (ra("tccr0b") >= 0) ? ra("tccr0b") : ra("tccr0");
        if (split || tn13) { int tb = tn13 ? 1 : 0;
            g_ts[0] = { 1, ra("tcnt0"), -1, tccr, ra("tifr0"), ra("timsk0"), tb, tb };
        } else g_ts[0] = { 1, ra("tcnt0"), -1, tccr, ra("tifr"), ra("timsk"), 0, 0 };
    }
    if (ra("tcnt1l") >= 0 && ra("tcnt1h") >= 0) {
        int tb = split ? 0 : 2;
        g_ts[1] = { 1, ra("tcnt1l"), ra("tcnt1h"), (int)ra("tccr1b"),
                    (int)(split ? ra("tifr1")  : ra("tifr")),
                    (int)(split ? ra("timsk1") : ra("timsk")), tb, tb };
    }
    if (ra("tcnt2") >= 0) {
        int tccr = (ra("tccr2b") >= 0) ? ra("tccr2b") : ra("tccr2");
        int tb = split ? 0 : 6;
        g_ts[2] = { 1, ra("tcnt2"), -1, tccr,
                    (int)(split ? ra("tifr2")  : ra("tifr")),
                    (int)(split ? ra("timsk2") : ra("timsk")), tb, tb };
    }

    resetInts();
}

// ---------------------------------------------------------------------
// Data memory + GPIO logic (README sec.8.1 rule: DDR=0 & PORT=1 -> PIN=1)
// ---------------------------------------------------------------------
static uint8_t readDmem(int a) {
    if (a < 0 || a >= (int)DMEM_SIZE) return 0;
    for (int i = 0; i < (int)g_pio.size(); i++) {
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
    if (a == g_udr && g_udr >= 0) {
        // ЧТЕНИЕ UDR (Terminal I/O): выдаём принятый байт, RXC сбрасывается;
        // если в RX-очереди ещё есть байт — подгружаем его в UDR и снова ставим RXC
        uint8_t c = g_dmem[g_udr];
        g_dmem[g_udr] = 0;
        if (g_ucsrA >= 0) g_dmem[g_ucsrA] &= ~0x80;             // RXC = 0
        if (!g_tioRx.empty()) {
            g_dmem[g_udr] = g_tioRx[0];
            g_tioRx.erase(g_tioRx.begin());
            if (g_ucsrA >= 0 && !g_tioRx.empty()) g_dmem[g_ucsrA] |= 0x80;   // RXC для очередного
        }
        return c;
    }
    return g_dmem[a];
}
static void writeDmem(int a, int val) {
    val &= 0xFF;
    if (a == g_udr && g_udr >= 0 && a < (int)DMEM_SIZE) {
        // Terminal I/O: запись в UDR = передача (как в AVR Studio)
        if ((int)g_tioTx.size() < 8192) g_tioTx += (char)val;
    }
    if (a == 0x20 + 0x3F) {
        g_f.I=(val>>7)&1; g_f.T=(val>>6)&1; g_f.H=(val>>5)&1; g_f.S=(val>>4)&1;
        g_f.V=(val>>3)&1; g_f.N=(val>>2)&1; g_f.Z=(val>>1)&1; g_f.C=val&1;
        return;
    }
    if (a == g_eeCR && !g_eeprom.empty() && g_eeARL >= 0 && g_eeARL < (int)DMEM_SIZE
        && g_eeDR >= 0 && g_eeDR < (int)DMEM_SIZE) {   // EECR/I адреса берём из профиля
        uint16_t ea = (uint16_t)g_dmem[g_eeARL];
        if (g_eeARH >= 0) ea |= (uint16_t)(g_dmem[g_eeARH] << 8);
        ea = (uint16_t)(ea % g_eeprom.size());
        if (val & 0x02) {                          // EEWE=1: commit EEDR into EEPROM cell
            g_eeprom[ea] = g_dmem[g_eeDR];
            val &= ~0x02;                          // write completes instantly -> EEWE cleared
        }
        if (val & 0x01) {                          // EERE=1: load EEDR from cell
            g_dmem[g_eeDR] = g_eeprom[ea];
            val &= ~0x01;                          // read done -> EERE cleared
        }
        g_dmem[a] = (uint8_t)val;
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
    int c = ((a + b + cin) > 0xFF) ? 1 : 0;
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

static void parseDwarfLine(const std::vector<uint8_t>& b, uint32_t off, uint32_t size);
static bool loadElfSys(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52 || b[0] != 0x7F || b[1] != 'E') { err = "not an ELF file"; return false; }

    g_flash.assign(FLASH_END, 0xFF);
    std::fill(g_dmem.begin(), g_dmem.end(), 0);
    for (int i = 0; i < (int)g_pio.size(); i++) { g_ext[i].mask = 0; g_ext[i].level = 0; }
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
    // DWARF .debug_line → (addr, line) table for line-exact stepping
    {
        uint32_t dshoff = rD(b, 32); uint16_t dshnum = rW(b, 48);
        uint16_t shstrIdx = rW(b, 0x32);
        int so = (int)dshoff + shstrIdx * 40;
        uint32_t strTabOff = rD(b, so + 16), strTabSize = rD(b, so + 20);
        for (int i = 0; i < (int)dshnum; i++) {
            int s = (int)dshoff + i * 40;
            if (s + 40 > (int)b.size()) break;
            uint32_t off = rD(b, s + 16), sz = rD(b, s + 20);
            uint32_t nameOff = rD(b, s + 0);
            std::string nm;
            if (nameOff < strTabSize) {
                uint32_t q = strTabOff + nameOff;
                while (q < b.size() && b[q] != 0) { nm += (char)b[q]; ++q; }
            }
            if (nm == ".debug_line") { parseDwarfLine(b, off, sz); break; }
        }
    }
    g_loaded = true;
    // power-on: EEPROM starts erased (0xFF) at every emulate/start cycle load
    std::fill(g_eeprom.begin(), g_eeprom.end(), 0xFF);
    return true;
}

// ---------------------------------------------------------------------
// DWARF .debug_line parser (DWARF v2/3/4): builds (byte-addr, line) rows
// so next/step/bp/until can stop on EXACT C source lines (README sec.12)
// ---------------------------------------------------------------------
static std::vector<uint32_t>   g_lnAddr;   // byte address in program space
static std::vector<int>        g_lnLine;
static std::vector<uint8_t>    g_lnEnd;    // end_sequence marker
static bool                    g_hasLines = false;

static uint64_t rdULEB(const std::vector<uint8_t>& b, uint32_t& p) {
    uint64_t r = 0; int sh = 0;
    for (;;) { uint8_t x = b[p++]; r |= (uint64_t)(x & 0x7F) << sh; sh += 7; if (!(x & 0x80)) break; }
    return r;
}
static int64_t rdSLEB(const std::vector<uint8_t>& b, uint32_t& p) {
    int64_t r = 0; int sh = 0; uint8_t x;
    do { x = b[p++]; r |= (int64_t)(x & 0x7F) << sh; sh += 7; } while (x & 0x80);
    if (sh < 64 && (x & 0x40)) r |= -(((int64_t)1) << sh);
    return r;
}
static void lnRow(uint32_t a, int l, uint8_t e) { g_lnAddr.push_back(a); g_lnLine.push_back(l); g_lnEnd.push_back(e); }

static void parseDwarfLine(const std::vector<uint8_t>& b, uint32_t off, uint32_t size) {
    g_lnAddr.clear(); g_lnLine.clear(); g_lnEnd.clear(); g_hasLines = false;
    uint32_t p = off, endP = off + size;
    if (endP > b.size()) endP = (uint32_t)b.size();
    while (p + 10 <= endP) {
        uint32_t uLen = rD(b, p); p += 4;
        if (uLen == 0xFFFFFFFFu) return;               // 64-bit DWARF: not supported
        if (p + uLen > endP) return;
        uint32_t uNext = p + uLen;
        uint16_t ver = rW(b, p); p += 2;
        if (ver < 2 || ver > 4) { p = uNext; continue; }
        p += 4;                                        // header_length
        uint32_t mInst = b[p++]; if (mInst == 0) mInst = 1;
        if (ver >= 4) p += 1;                          // max_ops_per_inst
        p += 1;                                        // default_is_stmt
        int lb = (int8_t)b[p++];                       // line_base
        int lr = b[p++]; if (lr == 0) lr = 1;          // line_range
        uint8_t ob = b[p++];                           // opcode_base
        p += (ob > 1) ? (ob - 1) : 0;                  // std_opcode_lengths
        while (p < uNext && b[p] != 0) { p++; } p++;    // include_directories (skip)
        p++;                                           // terminator
        while (p < uNext && b[p] != 0) {               // file_names (skip)
            while (p < uNext && b[p] != 0) p++; p++;
            (void)rdULEB(b, p); (void)rdULEB(b, p); (void)rdULEB(b, p);
        }
        if (p < uNext) p++;                            // file terminator
        uint32_t addr = 0; int line = 1;
        while (p < uNext) {
            uint8_t op = b[p++];
            if (op >= ob) {
                uint32_t adj = (uint32_t)(op - ob);
                addr += (adj / (uint32_t)lr) * mInst;
                line += lb + (int)(adj % (uint32_t)lr);
                lnRow(addr, line, 0);
                continue;
            }
            if (op == 0) {
                uint32_t len = (uint32_t)rdULEB(b, p);
                uint32_t q = p;
                uint8_t sub = (q < b.size()) ? b[q] : 0;
                if (sub == 1) { lnRow(addr, line, 1); addr = 0; line = 1; }
                else if (sub == 2) { addr = rD(b, q + 1); }
                // DW_LNE_define_file etc.: ignored
                p = q + len;               // ULEB len counts sub-opcode byte too
                continue;
            }
            switch (op) {
                case 1: lnRow(addr, line, 0); break;                        // copy
                case 2: addr += (uint32_t)rdULEB(b, p) * mInst; break;      // advance_pc
                case 3: line += (int)rdSLEB(b, p); break;                   // advance_line
                case 4: (void)rdULEB(b, p); break;                          // set_file
                case 5: (void)rdULEB(b, p); break;                          // set_column
                case 8: { uint32_t a2 = 255 - (uint32_t)ob; addr += (a2 / (uint32_t)lr) * mInst; } break; // const_add_pc
                case 9: addr += rW(b, p); p += 2; break;                    // fixed_advance_pc
                case 12: (void)rdULEB(b, p); break;                         // set_isa
                default: break;
            }
        }
        p = uNext;
    }
    if (!g_lnAddr.empty()) {
        // robust sort by address (stable)
        std::vector<uint32_t> idx(g_lnAddr.size());
        for (uint32_t i = 0; i < idx.size(); i++) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](uint32_t i, uint32_t j){ return g_lnAddr[i] < g_lnAddr[j]; });
        std::vector<uint32_t> a2; std::vector<int> l2; std::vector<uint8_t> e2;
        for (auto i : idx) { a2.push_back(g_lnAddr[i]); l2.push_back(g_lnLine[i]); e2.push_back(g_lnEnd[i]); }
        g_lnAddr.swap(a2); g_lnLine.swap(l2); g_lnEnd.swap(e2);
        g_hasLines = true;
    }
}

// last non-end row with addr <= A → its line (-1 none)
static int lineOfAddr(uint32_t a) {
    int best = -1;
    for (uint32_t i = 0; i < g_lnAddr.size(); i++)
        if (!g_lnEnd[i] && g_lnAddr[i] <= a) best = (int)i;
    return best >= 0 ? g_lnLine[best] : -1;
}
// first address whose line == LN
static uint32_t firstAddrOfLine(int ln) {
    for (uint32_t i = 0; i < g_lnAddr.size(); i++)
        if (!g_lnEnd[i] && g_lnLine[i] == ln) return g_lnAddr[i];
    return 0;
}
// smallest row address > A with a different source line
static uint32_t nextLineAddr(uint32_t a) {
    int cur = lineOfAddr(a);
    uint32_t best = 0;
    for (uint32_t i = 0; i < g_lnAddr.size(); i++) {
        if (g_lnEnd[i] || g_lnAddr[i] <= a) continue;
        if (g_lnLine[i] == cur) continue;
        if (best == 0 || g_lnAddr[i] < best) best = g_lnAddr[i];
    }
    return best;
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

// ---------------------------------------------------------------------
// SPM - Flash self-programming (README/Techn.md parity). Avr-libc
// sequence: buffer via SPMEN, erase via PGERS+SPMEN, commit via PGWRT+SPMEN;
// BLBSET+SPMEN reads a signature byte into R0.
// ---------------------------------------------------------------------

static void doSpm() {
    if (g_pagebuf.empty()) spmInit();
    uint8_t cr = readDmem(0x20 + 0x37);            // SPMCR/SPMCSR
    uint16_t z = (uint16_t)(g_dmem[30] | (g_dmem[31] << 8));
    bool acted = false;
    if (cr & 0x01) {                                // SPMEN: stage R1:R0 word (or read sig on BLBSET)
        if (cr & 0x08) {                            // BLBSET -> signature byte into R0
            g_dmem[0] = (z == 1) ? g_sig[1] : (z == 2) ? g_sig[2] : g_sig[0];
        } else {
            uint16_t off = (uint16_t)(z & (g_spmPageSize - 1));
            if ((off & 1) == 0) { g_pagebuf[off] = g_dmem[0]; g_pagebuf[off+1] = g_dmem[1]; }
        }
        acted = true;
    }
    if ((cr & 0x01) && (cr & 0x02)) {               // PGERS + SPMEN: erase whole page
        uint32_t pg = (z - (z & (g_spmPageSize - 1)));
        if (pg + g_spmPageSize <= FLASH_END) {
            for (uint32_t i = 0; i < g_spmPageSize; i++) g_flash[pg + i] = 0xFF;
        }
        acted = true;
    }
    if ((cr & 0x01) && (cr & 0x04)) {               // PGWRT + SPMEN: commit page buffer
        uint32_t pg = (z - (z & (g_spmPageSize - 1)));
        for (uint32_t i = 0; i < g_spmPageSize && pg + i < FLASH_END; i++)
            g_flash[pg + i] = g_pagebuf[i];
        acted = true;
    }
    if (acted) {
        // SPMEN/PGERS/PGWRT are self-clearing in emulation (operation completes instantly)
        writeDmem(0x20 + 0x37, cr & 0xF0);
    }
}

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
        int A = (op & 0xF) | (((op>>9)&3)<<4);   // A[5:4]=bits[10:9], bit8 is Rd[4]
        rd = (op>>4)&0x1F;
        g_dmem[rd] = readDmem(0x20 + A);
    }
    else if ((op & 0xF800) == 0xB800) {                            // OUT
        int A = (op & 0xF) | (((op>>9)&3)<<4);
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
        uint16_t z = g_dmem[0x1E] | (g_dmem[0x1F] << 8);
        nextPC = z / 2; cy = 3;
    }
    else if (op == 0x9409) {                                     // IJMP
        uint16_t z = g_dmem[0x1E] | (g_dmem[0x1F] << 8);
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
        rd = (op>>4)&0x1F; uint16_t y = g_dmem[0x1C] | (g_dmem[0x1D]<<8);
        g_dmem[rd] = readDmem(y); y++; g_dmem[0x1C] = y&0xFF; g_dmem[0x1D] = (y>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x900A) {                          // LD Rd, -Y
        rd = (op>>4)&0x1F; uint16_t y = (g_dmem[0x1C] | (g_dmem[0x1D]<<8)) - 1;
        g_dmem[0x1C] = y&0xFF; g_dmem[0x1D] = (y>>8)&0xFF; g_dmem[rd] = (uint8_t)readDmem(y); cy = 2;
    }
    else if ((op & 0xD200) == 0x8000) {                          // LDD Rd, Y+q / Z+q
        rd = (op>>4)&0x1F; int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        if (op & 8) { uint16_t y = g_dmem[0x1C] | (g_dmem[0x1D]<<8); g_dmem[rd] = readDmem(y + q); }
        else        { uint16_t z = g_dmem[0x1E] | (g_dmem[0x1F]<<8); g_dmem[rd] = readDmem(z + q); }
        cy = 2;
    }
    else if ((op & 0xFE0F) == 0x900F) {                          // POP
        rd = (op>>4)&0x1F; g_dmem[rd] = (uint8_t)popByte(); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x9004) {                          // LPM Rd, Z
        rd = (op>>4)&0x1F; uint16_t z = g_dmem[0x1E] | (g_dmem[0x1F]<<8);
        g_dmem[rd] = z < FLASH_END ? g_flash[z] : 0xFF; cy = 3;
    }
    else if ((op & 0xFE0F) == 0x9005) {                          // LPM Rd, Z+
        rd = (op>>4)&0x1F; uint16_t z = g_dmem[0x1E] | (g_dmem[0x1F]<<8);
        g_dmem[rd] = z < FLASH_END ? g_flash[z] : 0xFF; z++;
        g_dmem[0x1E] = z&0xFF; g_dmem[0x1F] = (z>>8)&0xFF; cy = 3;
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
        rr = (op>>4)&0x1F; uint16_t y = g_dmem[0x1C] | (g_dmem[0x1D]<<8);
        writeDmem(y, g_dmem[rr]); y++; g_dmem[0x1C] = y&0xFF; g_dmem[0x1D] = (y>>8)&0xFF; cy = 2;
    }
    else if ((op & 0xFE0F) == 0x920A) {                          // ST -Y, Rr
        rr = (op>>4)&0x1F; uint16_t y = (g_dmem[0x1C] | (g_dmem[0x1D]<<8)) - 1;
        g_dmem[0x1C] = y&0xFF; g_dmem[0x1D] = (y>>8)&0xFF; writeDmem(y, g_dmem[rr]); cy = 2;
    }
    else if ((op & 0xFE0F) == 0x920E) {                          // ST -Z, Rr
        rr = (op>>4)&0x1F; uint16_t z = (g_dmem[0x1E] | (g_dmem[0x1F]<<8)) - 1;
        g_dmem[0x1E] = z&0xFF; g_dmem[0x1F] = (z>>8)&0xFF; writeDmem(z, g_dmem[rr]); cy = 2;
    }
    else if ((op & 0xD200) == 0x8200) {                          // STD Y+q / Z+q, Rr
        rr = (op>>4)&0x1F; int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        if (op & 8) { uint16_t y = g_dmem[0x1C] | (g_dmem[0x1D]<<8); writeDmem(y + q, g_dmem[rr]); }
        else        { uint16_t z = g_dmem[0x1E] | (g_dmem[0x1F]<<8); writeDmem(z + q, g_dmem[rr]); }
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
    else if (op == 0x95E8) { doSpm(); cy = 2; }                  // SPM (before BSET mask: 0x95E8&0xFF0F==0x9408!)
    else if ((op & 0xFF0F) == 0x9408) {                          // BSET/BCLR s: s=[6:4], bit7: 0=set/1=clear
        setChipFlag((op>>4)&7, (op>>7)&1 ? 0 : 1);
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
        snprintf(b, sizeof b, "LDD  R%u, %c+%d", (op>>4)&0x1F, (op&8)?'Y':'Z', q); return b;
    }
    if ((op & 0xD200) == 0x8200) {
        int q = ((op&0x2000)>>8) | ((op&0x0C00)>>7) | (op&7);
        snprintf(b, sizeof b, "%s %c+%d, R%u", "STD", (op&8)?'Y':'Z', q, (op>>4)&0x1F); return b;
    }
    if ((op & 0xFF0F) == 0x9408 || (op & 0xFF0F) == 0x9488) {
        static const char* fn[8] = {"C","Z","N","V","S","H","T","I"};
        snprintf(b, sizeof b, "%s%s (flag %s)", (op>>7)&1 ? "CL" : "SE", fn[(op>>4)&7], fn[(op>>4)&7]); return b;
    }
    if (op == 0x95E8) return "SPM";
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
        case 0xB000: if (op & 0x0800) snprintf(b, sizeof b, "OUT  0x%02X, R%u", (op&0xF)|((op>>9)&3)<<4, (op>>4)&0x1F);
                     else            snprintf(b, sizeof b, "IN   R%u, 0x%02X", (op>>4)&0x1F, (op&0xF)|((op>>9)&3)<<4); return b;
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
    for (int i = 0; i < (int)g_pio.size(); i++) {
        g_prev[fmt("DDR%s", g_pname[i].c_str())] = g_dmem[0x20 + g_pio[i].ddr];
        g_prev[fmt("PORT%s", g_pname[i].c_str())] = g_dmem[0x20 + g_pio[i].port];
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
    wr(fmt(" [X: 0x%04X]  [Y: 0x%04X]  [Z: 0x%04X]  (указатели R26..R31)",
          g_dmem[0x1A] | (g_dmem[0x1B] << 8), g_dmem[0x1C] | (g_dmem[0x1D] << 8), g_dmem[0x1E] | (g_dmem[0x1F] << 8)), CYAN);
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
    for (int i = 0; i < (int)g_pio.size(); i++) {
        uint8_t ddr = g_dmem[0x20+g_pio[i].ddr], prt = g_dmem[0x20+g_pio[i].port];
        uint8_t pin = readDmem(0x20+g_pio[i].pin);
        char ddrb[16], prtb[16], pinb[16];
        for (int b = 0; b < 8; b++) { ddrb[7-b] = ((ddr>>b)&1)+'0'; prtb[7-b] = ((prt>>b)&1)+'0'; pinb[7-b] = ((pin>>b)&1)+'0'; }
        ddrb[8] = prtb[8] = pinb[8] = 0;
        bool ch = g_prevValid && g_prev.count(fmt("DDR%s", g_pname[i].c_str())) && g_prev[fmt("DDR%s", g_pname[i].c_str())] != ddr;
        wr(fmt(" [PORT%s] DDR%s: 0b%s (0x%02X) | PORT%s: 0b%s (0x%02X) | PIN%s: 0b%s (0x%02X)",
               g_pname[i].c_str(), g_pname[i].c_str(), ddrb, ddr, g_pname[i].c_str(), prtb, prt, g_pname[i].c_str(), pinb, pin),
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
    g_flash.assign(FLASH_END, 0xFF);
    memcpy(g_flash.data(), prog, sizeof(prog));
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
        0x00,0x92,   // STS 0x61, R0
        0x61,0x00,
        0x40,0x91,   // LDS R20, 0x61 -> 0xF1
        0x61,0x00,
        0xE0,0xE6,   // LDI R30,0x60 (Z low)
        0xF0,0xE0,   // LDI R31,0    (Z high)
        0x12,0x82,   // STD Z+2, R1  -> [0x62] = 0x0E
        0x51,0x81,   // LDD R21, Z+1 <- [0x61] = 0xF1
        0x62,0x81 }; // LDD R22, Z+2 <- [0x62] = 0x0E
    g_flash.assign(FLASH_END, 0xFF);
    memcpy(g_flash.data(), prog, sizeof(prog));
    std::fill(g_dmem.begin(), g_dmem.end(), 0);
    g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END>>8)&0xFF;
    g_loaded = true; g_pc = 0; g_cycles = 0; g_f = {};

    uint16_t op; int cy;
    for (int i = 0; i < 6; i++) execOne(op, cy);          // LDI..SBRS (skips w6)
    tc("SEC (BSET0) C=1", g_f.C, 1);
    tc("BST R16,3 -> T=1", g_f.T, 1);
    tc("BLD R17,5 -> 0x20", g_dmem[17], 0x20);
    tc("SBRS skipped LDI (R18=0)", g_dmem[18], 0);
    execOne(op, cy);                                     // MOVW
    tc("MOVW R18 = 0xFF", g_dmem[18], 0xFF);
    tc("MOVW R19 = 0x20", g_dmem[19], 0x20);
    execOne(op, cy); execOne(op, cy);                    // LDI R24, MUL
    tc("MUL R0 = 0xF1", g_dmem[0], 0xF1);
    tc("MUL R1 = 0x0E", g_dmem[1], 0x0E);
    execOne(op, cy);                                     // STS 0x61, R0
    execOne(op, cy);                                     // LDS R20
    tc("LDS/STS R20 = 0xF1", g_dmem[20], 0xF1);
    execOne(op, cy);
    execOne(op, cy);                                     // Z = 0x60 (LDI R30/R31)
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

// ---------------------------------------------------------------------
// Interrupt dispatch (README sec.5.2): runs once per executed instruction.
//  - Timer overflows: real TCNT0/1/2 counters tick per prescaler cycles
//    (TCCRn CS bits), overflow sets TOVn in TIFR(n).
//  - External INT0/INT1: edge detector on the INT pin (MCUCR/EICRA ISC bits),
//    event sets INTFn in GIFR/EIFR.
//  - Pending sources (enable && flag, or low-level for INT) call the ISR:
//    push return PC, I=0, PC=vector, +4 cycles (hardware entry cost).
// Vectors/addresses are taken from the chip profile, sorted by priority
// (ascending vector index). RETI restores I=1 (execOne).
// ---------------------------------------------------------------------
static void tickTimers(int cy) {
    if (cy <= 0) return;
    for (int t = 0; t < 3; t++) {
        if (!g_ts[t].have) continue;
        int cs = readDmem(g_ts[t].tccr) & 7;
        static const int base[8] = { 0, 1, 8, 64, 256, 1024, 0, 0 };   // T0/T1 (CS 6..7 = ext pin: not emulated)
        static const int t2tb[8] = { 0, 1, 8, 32, 64, 128, 256, 1024 }; // T2
        int n = (t == 2) ? t2tb[cs] : base[cs];
        if (n <= 0) { g_tAcc[t] = 0; continue; }                       // stopped
        g_tAcc[t] += (uint64_t)cy;
        while (g_tAcc[t] >= (uint64_t)n) {
            g_tAcc[t] -= (uint64_t)n;
            uint8_t wasL = (uint8_t)readDmem(g_ts[t].tcntL);
            uint8_t isL  = (uint8_t)(wasL + 1);
            writeDmem(g_ts[t].tcntL, isL);
            bool ovf = (isL == 0);                                     // 8-bit wrap
            if (g_ts[t].tcntH >= 0) {                                  // 16-bit: tick high byte on carry
                uint8_t wasH = (uint8_t)readDmem(g_ts[t].tcntH);
                uint8_t isH  = (uint8_t)(wasH + 1);
                if (isL == 0) { writeDmem(g_ts[t].tcntH, isH); ovf = (isH == 0); }
                else ovf = false;
            }
            if (ovf) writeDmem(g_ts[t].tifr, readDmem(g_ts[t].tifr) | (1 << g_ts[t].tovBit));
        }
    }
}

static void tickExtEdge() {
    if (g_isrv.empty()) return;
    // ISC register / flag reg / enable reg per chip family (from regmap names)
    bool hasEimsk = g_regmap.count("eimsk") > 0;               // modern m88/168/328 family
    bool hasTf0   = g_regmap.count("tifr0") > 0 && g_regmap.count("tifr1") == 0;   // tn13 (single INT0)
    int iscIO  = (hasEimsk && g_regmap.count("eicra")) ? g_regmap["eicra"] : 0x55;   // MCUCR = IO 0x35
    int flagIO = hasEimsk ? ((g_regmap.count("eifr"))  ? g_regmap["eifr"] : 0x3C)    // EIFR fallback (IO 0x1C)
                          : 0x5A;                       // GIFR = IO 0x3A (m8/16/32, tn13 alike)
    struct X { int pinIO, bit, flagBit, enBit; const char* vec; };
    X e[2] = { { g_int0PinIO, g_int0Bit, 0, 0, "int0_vect" },
               { g_int1PinIO, g_int1Bit, 0, 0, "int1_vect" } };
    if (hasEimsk) { e[0].flagBit = 0; e[0].enBit = 0; e[1].flagBit = 1; e[1].enBit = 1; }   // EIFR/EIMSK
    else          { e[0].flagBit = 6; e[0].enBit = 6; e[1].flagBit = 7; e[1].enBit = 7; }   // GIFR/GICR
    uint8_t isc = readDmem(iscIO);
    for (int i = 0; i < 2; i++) {
        if (!g_isrv.count(e[i].vec)) continue;
        if (i == 1 && hasTf0) continue;                        // tiny13 has no INT1
        uint8_t cur = (uint8_t)((readDmem(0x20 + e[i].pinIO) >> e[i].bit) & 1);
        int m = (i == 0) ? 0 : 2;
        if (g_edgeInit) {
            uint8_t prev = g_edgeSt[i];
            int mode = (isc >> m) & 3;
            bool hit = (mode == 1) ? (prev != cur)                                    // any logical change
                     : (mode == 2) ? (prev == 1 && cur == 0)                          // falling edge
                     : (mode == 3) ? (prev == 0 && cur == 1)                          // rising edge
                     : false;                                                          // low level -> dispatch only
            if (hit) writeDmem(flagIO, readDmem(flagIO) | (1 << e[i].flagBit));
        }
        g_edgeSt[i] = cur;
    }
    g_edgeInit = true;
}

static void tickInts(int cy) { tickTimers(cy); tickExtEdge(); }

// Returns true when an ISR was entered (PC now points at the vector).
static bool dispatchPending() {
    if (!g_f.I || g_isrv.empty()) return false;
    bool hasEimsk = g_regmap.count("eimsk") > 0;
    bool hasTf0   = g_regmap.count("tifr0") > 0 && g_regmap.count("tifr1") == 0;
    struct Src { int vec; std::string name; bool active; int flagIO, flagBit; };
    std::vector<Src> list;
    int iscIO  = (hasEimsk && g_regmap.count("eicra")) ? g_regmap["eicra"] : 0x55;
    int flagIO = hasEimsk ? ((g_regmap.count("eifr"))  ? g_regmap["eifr"] : 0x3C) : 0x5A;
    int enIO   = hasEimsk ? ((g_regmap.count("eimsk")) ? g_regmap["eimsk"] : 0x3D)
                          : (hasTf0 ? ((g_regmap.count("gimsk")) ? g_regmap["gimsk"] : 0x5B)
                                    : ((g_regmap.count("gicr"))    ? g_regmap["gicr"] : 0x5B));
    struct X { int pinIO, bit, flagBit, enBit; const char* vec; };
    X e[2] = { { g_int0PinIO, g_int0Bit, 0, 0, "int0_vect" },
               { g_int1PinIO, g_int1Bit, 0, 0, "int1_vect" } };
    if (hasEimsk) { e[0].flagBit = 0; e[0].enBit = 0; e[1].flagBit = 1; e[1].enBit = 1; }
    else          { e[0].flagBit = 6; e[0].enBit = 6; e[1].flagBit = 7; e[1].enBit = 7; }
    uint8_t isc = readDmem(iscIO);
    for (int i = 0; i < 2; i++) {
        auto vi = g_isrv.find(e[i].vec);
        if (vi == g_isrv.end()) continue;
        if (i == 1 && hasTf0) continue;
        int m = (i == 0) ? 0 : 2;
        int mode = (isc >> m) & 3;
        bool active;
        if (mode == 0) active = (readDmem(0x20 + e[i].pinIO) >> e[i].bit & 1) == 0;   // low level while held
        else           active = (readDmem(flagIO) >> e[i].flagBit) & 1;
        if (!active) continue;
        uint8_t en = readDmem(enIO);
        if (!((en >> e[i].enBit) & 1)) continue;
        list.push_back({ vi->second, e[i].vec, true, flagIO, e[i].flagBit });
    }
    // USART приём (Terminal I/O): RXC=1 и RXCIE=1 -> ISR USART RX (как в AVR Studio)
    if (g_udr >= 0 && g_ucsrA >= 0 && g_ucsrB >= 0 && g_udr < (int)DMEM_SIZE) {
        static const char* cn[4] = { "usart_rx_vect", "usart_rxc_vect", "usart0_rx_vect", "usart0_rxc_vect" };
        auto vi = g_isrv.end();
        for (int k = 0; k < 4 && vi == g_isrv.end(); k++) vi = g_isrv.find(cn[k]);
        if (vi != g_isrv.end() && (readDmem(g_ucsrA) >> 7) && (readDmem(g_ucsrB) >> 7))
            list.push_back({ vi->second, vi->first, true, g_ucsrA, 7 });
    }
    for (int t = 0; t < 3; t++) {
        if (!g_ts[t].have) continue;
        const char* names[2] = { t == 0 ? "timer0_ovf_vect" : t == 1 ? "timer1_ovf_vect" : "timer2_ovf_vect",
                                 t == 0 ? "tim0_ovf_vect"   : t == 1 ? "tim1_ovf_vect"   : "tim2_ovf_vect" };
        for (int k = 0; k < 2 && k < (t == 1 ? 1 : 2); k++) {
            auto vi = g_isrv.find(names[k]);
            if (vi == g_isrv.end()) continue;
            uint8_t f = readDmem(g_ts[t].tifr), en = readDmem(g_ts[t].timsk);
            if (((f >> g_ts[t].tovBit) & 1) && ((en >> g_ts[t].toieBit) & 1))
                list.push_back({ vi->second, names[k], true, g_ts[t].tifr, g_ts[t].tovBit });
            break;
        }
    }
    if (list.empty()) return false;
    // hardware priority: lowest vector number first
    size_t best = 0;
    for (size_t i = 1; i < list.size(); i++) if (list[i].vec < list[best].vec) best = i;
    Src& s = list[best];
    pushWord(g_pc);                 // return address (next instruction)
    g_f.I = 0;                      // ISR runs with interrupts off
    g_pc = (uint16_t)s.vec;         // vector slot (PC word == vector index)
    g_cycles += 4;                  // interrupt entry costs 4 cycles
    // acknowledge: clear the pending flag (edge/overflow sources; low-level re-arms while held)
    writeDmem(s.flagIO, readDmem(s.flagIO) & ~(1 << s.flagBit));
    if (!g_quiet) wr(fmt("-- Interrupt: %s -> PC=0x%04X (cycle %llu)", s.name.c_str(), g_pc, (unsigned long long)g_cycles), MAGENTA);
    return true;
}

// ---------------------------------------------------------------------
// Shared runner: executes until a user bp / temp bp / maxCycles.
// Returns: 0=hit user bp, 1=hit temp bp, -1=stopped (max/EOF)
// ---------------------------------------------------------------------
static int runUntil(uint64_t maxC, const char* tag) {
    uint64_t cnt = 0;
    g_running = true;
    while (cnt < maxC && g_running) {
        if (!g_bpTemp.empty() && g_bpTemp.count(g_pc)) {
            wr(fmt("-- %s at PC=0x%04X (cycle %llu)", tag, g_pc, (unsigned long long)g_cycles), GREEN);
            g_running = false; return 1;
        }
        auto bi = g_bp.find(g_pc);
        if (bi != g_bp.end()) {
            wr(fmt("-- Breakpoint #%d hit at PC=0x%04X (cycle %llu)", bi->second, g_pc, (unsigned long long)g_cycles), RED);
            g_running = false; return 0;
        }
        uint16_t op; int cy;
        if (!execOne(op, cy)) break;
        cnt++;
        tickInts(cy);
        if (dispatchPending()) cnt++;
    }
    g_running = false;
    return -1;
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
                resetInts();
                snapPrev(); renderDash(); wr("Reset done.", GREEN); return true;
            }
            if (loadElfSys(g_elf, err)) { snapPrev(); wr("Loaded: " + g_elf, GREEN); renderDash(); }
            else wr("Load failed: " + err, RED);
        } else if (s1 == "stop") { g_running = false; }
        return true;
    }
    if (cmd == "load" || cmd == "emu.start") {
        std::string err;
        if (loadElfSys(g_elf, err)) { resetInts(); snapPrev(); wr("Loaded: " + g_elf, GREEN); renderDash(); }
        else wr("Load failed: " + err, RED);
        return true;
    }
    if (cmd == "reset" || cmd == "rst") {
        g_pc = 0; g_cycles = 0; g_running = false;
        std::fill(g_dmem.begin(), g_dmem.end(), 0);
        for (int i = 0; i < (int)g_pio.size(); i++) { g_ext[i].mask = 0; g_ext[i].level = 0; }
        g_dmem[0x5D] = RAM_END & 0xFF; g_dmem[0x5E] = (RAM_END>>8)&0xFF;
        resetInts();
        snapPrev(); renderDash(); wr("Reset done.", GREEN); return true;
    }
    if (cmd == "stepi" || cmd == "si") {
        int n = 1;
        if (!a(1).empty()) { bool ok; n = parseNum(a(1), ok); if (!ok) n = 1; }
        if (!g_loaded) { wr("! No program loaded. Use: load <elf>", RED); return true; }
        snapPrev();
        for (int i = 0; i < n; i++) {
            uint16_t op; int cy;
            if (!execOne(op, cy)) break;
            tickInts(cy);
            dispatchPending();
            renderDash();
        }
        return true;
    }
    // Line-step (README sec.12: next = "over" a C;line; step = to the next line,
    // entering functions). Implemented EXACTLY through the DWARF .debug_line
    // table parsed from the ELF. Without -g: fallback (over call / 1 instr).
    if (cmd == "step" || cmd == "s" || cmd == "next" || cmd == "n") {
        if (!g_loaded) { wr("! No program loaded. Use: load <elf>", RED); return true; }
        int n = 1;
        if (!a(1).empty()) { bool ok; n = parseNum(a(1), ok); if (!ok) n = 1; }
        bool over = (cmd == "next" || cmd == "n");
        g_bpTemp.clear(); snapPrev();
        if (!g_hasLines) wr("(ELF without -g: line mode unavailable, instruction fallback)", YELLOW);
        for (int i = 0; i < n; i++) {
            uint32_t trg = 0; bool lineMode = false;
            if (g_hasLines) {
                int L = lineOfAddr(g_pc * 2u);
                if (L > 0) {
                    uint32_t nb = nextLineAddr(g_pc * 2u);
                    if (nb) { trg = nb / 2u; lineMode = true; }
                }
            }
            if (lineMode) {
                if (over) {                                  // do not enter the call
                    uint16_t cur = flashWord(g_pc);
                    if (isCallOp(cur)) {
                        uint16_t tPC = (uint16_t)(g_pc + isCall2Word(cur));
                        g_bpTemp[tPC] = 1;
                        runUntil(10000000, "Step over");
                        g_bpTemp.clear();
                        continue;
                    }
                }
                g_bpTemp[trg] = 1;
                runUntil(10000000, over ? "Next line" : "Step into");
                g_bpTemp.clear();
            } else {
                uint16_t cur = flashWord(g_pc);
                if (over && isCallOp(cur)) {
                    uint16_t tPC = (uint16_t)(g_pc + isCall2Word(cur));
                    g_bpTemp[tPC] = 1;
                    runUntil(10000000, "Step over");
                    g_bpTemp.clear();
                } else {
                    uint16_t op; int cy;
                    if (!execOne(op, cy)) break;
                    tickInts(cy);
                    dispatchPending();
                }
            }
            renderDash();
        }
        return true;
    }
    // finish (fin): run until the current subroutine returns (SP returns to entry level)
    if (cmd == "finish" || cmd == "fin") {
        if (!g_loaded) { wr("! No program loaded. Use: load <elf>", RED); return true; }
        uint16_t spEntry = g_dmem[0x5D] | (g_dmem[0x5E]<<8);
        if (g_bpTemp.count(g_pc)) { g_bpTemp.clear(); }
        snapPrev();
        wr(fmt("-- finish: from PC=0x%04X, SP_entry=0x%04X", g_pc, spEntry), CYAN);
        uint64_t cnt = 0; g_running = true;
        bool below = false; bool hitRet = false;
        while (g_running && cnt < 50000000) {
            auto bi = g_bp.find(g_pc);
            if (bi != g_bp.end()) { wr(fmt("-- Breakpoint #%d hit at PC=0x%04X", bi->second, g_pc), RED); g_running=false; break; }
            uint16_t op; int cy;
            if (!execOne(op, cy)) break;
            cnt++;
            tickInts(cy);
            if (dispatchPending()) cnt++;
            uint16_t spNow = g_dmem[0x5D] | (g_dmem[0x5E]<<8);
            hitRet = (op == 0x9508 || op == 0x9518);
            if (spNow < spEntry) { below = true; }
            else if (below || (hitRet && spNow >= spEntry)) {
                wr(fmt("-- Returned from subroutine at PC=0x%04X (cycle %llu)", g_pc, (unsigned long long)g_cycles), GREEN);
                g_running=false; break;
            }
        }
        g_running = false;
        if (cnt >= 50000000) wr("-- finish: max-cycles limit hit.", YELLOW);
        renderDash();
        return true;
    }
    // until (u): run to line / address / symbol (temporary breakpoint).
    // A plain decimal number = SOURCE LINE when the ELF has debug info
    // (README sec.12), otherwise = word address. '0x..': always address.
    if (cmd == "until" || cmd == "u") {
        if (!g_loaded) { wr("! No program loaded. Use: load <elf>", RED); return true; }
        std::string tgt = a(1);
        if (tgt.empty()) {
            wr(g_hasLines
                ? "Usage: until <line | 0xaddr | symbol>   (e.g. until 34 / until main)"
                : "Usage: until <0xaddr | symbol>   (rebuild with -g for line support)", RED);
            return true;
        }
        uint32_t n = 0; bool ok = false; bool asLine = false; int lnVal = 0;
        bool hexLike = (tgt.size() > 1 && (tgt[1] == 'x' || tgt[1] == 'X'));
        if ((isdigit((unsigned char)tgt[0]) || tgt[0]=='0') && !hexLike && g_hasLines) {
            bool isNum; int ln = parseNum(tgt, isNum);
            if (isNum && ln >= 1) {
                uint32_t ba = firstAddrOfLine(ln);
                if (ba) { n = ba / 2u; ok = true; asLine = true; lnVal = ln; }
            }
        }
        if (!ok && !asLine && (isdigit((unsigned char)tgt[0]) || tgt[0]=='0')) {
            n = (uint32_t)parseNum(tgt, ok);
        }
        uint32_t sym = 0;
        if (!ok && findSymbol(tgt, sym)) { n = sym / 2; ok = true; }
        if (!ok) {
            wr(fmt("until: cannot resolve '%s' (line needs -g in the ELF; %s)",
                   tgt.c_str(), g_hasLines ? "always '0x' for addresses" : "no debug lines found"),
               RED);
            return true;
        }
        if (asLine) wr(fmt("-- until: line %d -> PC=0x%04X", lnVal, (uint16_t)n), DCYAN);
        uint16_t pc = (uint16_t)n;
        g_bpTemp[pc] = 1; snapPrev();
        int res = runUntil(50000000, "Run to");
        g_bpTemp.clear();
        if (res != 1) wr(fmt("-- until: target PC=0x%04X not reached (max-cycles).", pc), YELLOW);
        renderDash();
        return true;
    }
    if (cmd == "run" || cmd == "r") {
        if (!g_loaded) { wr("! No program loaded. Use: emu start", RED); return true; }
        uint64_t maxC = 100000000;   // ~12.5 s of program time at 8 MHz (one _delay_ms(500) = 4M cycles)
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
            tickInts(cy);
            if (dispatchPending()) cnt++;
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
        } else if (s1[0] == 'r') {
            bool ok; int idx = parseNum(s1.substr(1), ok);
            if (ok && idx >= 0 && idx < 32) {
                // assign syntaxes: reg R16 = 0x55 | reg R16 0x55 | reg R16 = -25
                std::string valTok = a(2);
                if (valTok == "=" && !a(3).empty()) valTok = a(3);
                if (!valTok.empty()) {
                    bool ok2; int v = parseNum(valTok, ok2);
                    if (ok2) { g_dmem[idx] = (uint8_t)(v & 0xFF); wr(fmt(" R%02d := 0x%02X", idx, (uint8_t)v), GREEN); }
                    else wr(fmt(" ! Bad value: %s", valTok.c_str()), RED);
                }
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
        int pi = portIndex(p.empty() ? '\0' : p[0]);
        if (pi < 0) {
            std::string av; for (size_t k = 0; k < g_pname.size(); k++) { av += g_pname[k]; if (k+1 < g_pname.size()) av += "|"; }
            if (av.empty()) av = "<none>";
            wr("Usage: port <" + av + ">   (chip " + g_chip + " has these ports)", RED);
            return true;
        }
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
            int pi = portIndex(pn[1]);
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
        if (region.empty()) { wr("Usage: mem <sram|flash|eeprom> <addr> [len]   |   write: mem sram 0x60 = 0xFF", RED); return true; }
        if (region == "eeprom" && g_eeprom.empty()) { wr(" EEPROM size = 0 (no EepromEnd in chip profile).", RED); return true; }
        // region byte accessors
        auto rGet = [&](uint32_t aa) -> int {
            if (region == "flash")   return (aa < FLASH_END) ? g_flash[aa] : 0xFF;
            if (region == "eeprom")  return (aa < g_eeprom.size()) ? g_eeprom[aa] : 0xFF;
            return aa < (uint32_t)DMEM_SIZE ? readDmem((int)aa) : 0;
        };
        auto rSet = [&](uint32_t aa, uint8_t v) {
            if (region == "flash")   { if (aa < FLASH_END)     { g_flash[aa] = v;  return true; } }
            else if (region == "eeprom") { if (aa < g_eeprom.size()) { g_eeprom[aa] = v; return true; } }
            else { if (aa < (uint32_t)DMEM_SIZE) { writeDmem((int)aa, v); return true; } }
            return false;
        };
        // write mode: "mem sram 0x60 = 0xFF  [next val]"
        for (int t = 2; t <= 6; t++) {
            if (a(t) == "=" && !a(t+1).empty()) {
                bool okA, okV; uint32_t wa = parseNum(a(t-1), okA); int v = parseNum(a(t+1), okV);
                if (!okA || !okV) { wr(" ! Bad address/value in mem write.", RED); return true; }
                uint32_t wai = wa;
                int written = 0;
                for (;;) {
                    if (rSet(wai, (uint8_t)v)) { wr(fmt(" %s 0x%04X := 0x%02X", upper(region).c_str(), wai, (uint8_t)v), GREEN); }
                    else { wr(fmt(" ! %s address 0x%04X out of range.", upper(region).c_str(), wai), RED); }
                    written++;
                    bool hasNext = (written == 1) && !a(t+2).empty();
                    if (!hasNext) break;
                    bool okV2; int v2 = parseNum(a(t+2), okV2);
                    if (!okV2) break;
                    v = v2; wai++;
                    t += 1;
                }
                return true;
            }
        }
        bool ok; uint32_t addr = a(2).empty() ? 0 : parseNum(a(2), ok);
        int len = 16; if (!a(3).empty()) { bool ok3; len = parseNum(a(3), ok3); }
        for (int i = 0; i < len; i += 8, addr += 8) {
            char hx[32]; int w = 0;
            for (int j = 0; j < 8 && (i+j) < len; j++) {
                w += snprintf(hx + w, sizeof(hx)-w, "%02X ", rGet((uint32_t)((int)addr + j)));
            }
            wr(fmt(" 0x%04X: %s", addr, hx));
        }
        return true;
    }
    if (cmd == "w" || cmd == "watch") {
        std::string t = a(1);
        if (!t.empty()) {
            bool ok; int v = parseNum(t, ok);
            if (!ok) { uint32_t sym = 0; if (findSymbol(t, sym)) { v = (int)(sym & 0xFFFF); ok = true; } }
            if (ok) { g_watch.push_back({t, (uint16_t)v}); wr(fmt("Watch added: %s @ 0x%04X", t.c_str(), v), GREEN); }
            else wr(fmt(" ! Watch: cannot resolve '%s' (variable or address expected).", t.c_str()), RED);
        } else wr("Usage: watch <variable | sram-addr>   (watch off - Wenzel)", RED);
        return true;
    }
    if (cmd == "bp") {
        std::string s1 = lower(a(1));
        if (s1 == "add" || s1 == "ba") {
            uint32_t addr = 0; bool ok = true;
            uint16_t pc = 0xFFFF;
            if (a(2).empty()) ok = false;
            else {
                std::string t = a(2);
                bool hexLike = (t.size() > 1 && (t[1] == 'x' || t[1] == 'X'));
                bool asLine = false;
                if (!hexLike && isdigit((unsigned char)t[0]) && g_hasLines) {
                    bool isNum; int ln = parseNum(t, isNum);
                    if (isNum && ln >= 1) {
                        uint32_t ba = firstAddrOfLine(ln);
                        if (ba) { pc = (uint16_t)(ba / 2u); ok = true; asLine = true;
                                  wr(fmt("-- bp: line %d -> PC=0x%04X", ln, pc), DCYAN); }
                    }
                }
                if (!asLine) {
                    if (isdigit((unsigned char)t[0]) || t[0]=='0') {
                        bool o2; addr = parseNum(t, o2); ok = o2; pc = addr;   // word address
                    } else {
                        uint32_t sym = 0;
                        if (findSymbol(t, sym)) { ok = true; pc = (uint16_t)(sym / 2); }
                        else if (g_isrv.count(lower(t))) {
                            // vector name from the chip profile [Interrupts] (e.g. TIMER1_OVF_vect)
                            pc = (uint16_t)g_isrv[lower(t)]; ok = true;
                            wr(fmt("-- bp: vector %s -> PC=0x%04X", t.c_str(), pc), DCYAN);
                        }
                        else { ok = false; wr(fmt("bp add: cannot resolve symbol '%s'", t.c_str()), RED); }
                    }
                }
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
    if (cmd == "stk" || cmd == "stack") {
        uint16_t sp = g_dmem[0x5D] | (g_dmem[0x5E] << 8);
        wr(fmt(" SP=0x%04X, RAM_END=0x%04X, стек растёт вверх:", sp, RAM_END), CYAN);
        int n = 0;
        for (long a0 = sp + 1; a0 + 1 <= RAM_END && n < 8; a0 += 2, n++) {
            uint16_t w = readDmem((int)a0) | (readDmem((int)a0 + 1) << 8);
            wr(fmt("  [0x%04X] 0x%04X%s", (int)a0, w, (n == 0) ? "   <- вершина" : ""));
        }
        if (n == 0) wr("  (стек пуст)");
        return true;
    }
    if (cmd == "regs" || cmd == "regm") {
        // карта регистров текущего профиля [RegisterMap] с фильтром по имени
        std::string flt = lower(a(1));
        int shown = 0;
        for (auto& kv : g_regmap) {
            if (!flt.empty() && kv.first.find(flt) == std::string::npos) continue;
            int s = kv.second; int io = s - 0x20;
            if (io >= 0 && io <= 0x3F) wr(fmt("  %-14s IO 0x%02X / SRAM 0x%04X", upper(kv.first).c_str(), io, s));
            else                       wr(fmt("  %-14s SRAM 0x%04X (extended)", upper(kv.first).c_str(), s));
            shown++;
        }
        wr(fmt("  найдено: %d.  подробнее: io <REG>", shown), DGRAY);
        return true;
    }
    if (cmd == "io" || cmd == "iov") {
        // I/O-view побитно, как I/O window в AVR Studio (берём адрес из профиля)
        std::string rn = upper(a(1));
        if (rn.empty()) { wr(" формат: io <REG> (io SREG / io TIMSK); список: regs [фильтр]", YELLOW); return true; }
        auto it = g_regmap.find(lower(rn));
        if (it == g_regmap.end()) { wr(" Регистр " + rn + " не найден в профиле [RegisterMap]", RED); return true; }
        int s = it->second; int ioA = s - 0x20;
        uint8_t v = (uint8_t)readDmem(s);
        char bin[9]; for (int b = 0; b < 8; b++) bin[b] = ((v >> (7 - b)) & 1) ? '1' : '0'; bin[8] = 0;
        std::string ios = (ioA >= 0 && ioA <= 0x3F) ? fmt("  IO 0x%02X", ioA) : "";
        wr(fmt(" %s @ SRAM 0x%04X%s = 0x%02X  0b%s", rn.c_str(), s, ios.c_str(), v, bin), CYAN);
        static const struct { const char* nm; const char* bl[8]; } bt[] = {
            { "SREG",   { "I", "T", "H", "S", "V", "N", "Z", "C" } },
            { "MCUSR",  { "", "", "", "", "WDRF", "BORF", "EXTRF", "PORF" } },
            { "MCUCSR", { "JTD", "", "", "", "WDRF", "BORF", "EXTRF", "PORF" } },
            { "SPMCSR", { "SPMIE", "RWWSB", "", "RWWSRE", "BLBSET", "PGWRT", "PGERS", "SPMEN" } },
            { "SPMCR",  { "SPMIE", "RWWSB", "", "RWWSRE", "BLBSET", "PGWRT", "PGERS", "SPMEN" } },
            { "ADCSRA", { "ADEN", "ADSC", "ADATE", "ADIF", "ADIE", "ADPS2", "ADPS1", "ADPS0" } },
            { "ADMUX",  { "REFS1", "REFS0", "ADLAR", "", "MUX3", "MUX2", "MUX1", "MUX0" } },
            { "UCSRA",  { "RXC", "TXC", "UDRE", "FE", "DOR", "UPE", "U2X", "MPCM" } },
            { "UCSRB",  { "RXCIE", "TXCIE", "UDRIE", "RXEN", "TXEN", "UCSZ2", "RXB8", "TXB8" } },
            { "UCSRC",  { "URSEL", "UMSEL", "UPM1", "UPM0", "USBS", "UCSZ1", "UCSZ0", "UCPOL" } },
            { "SPCR",   { "SPIE", "SPE", "DORD", "MSTR", "CPOL", "CPHA", "SPR1", "SPR0" } },
            { "SPSR",   { "SPIF", "WCOL", "", "", "", "", "", "SPI2X" } },
            { "TWSR",   { "TWS7", "TWS6", "TWS5", "TWS4", "TWS3", "", "TWPS1", "TWPS0" } },
            { "TCCR1A", { "COM1A1", "COM1A0", "COM1B1", "COM1B0", "", "WGM11", "WGM10", "" } },
            { "TCCR1B", { "ICNC1", "ICES1", "", "WGM13", "WGM12", "CS12", "CS11", "CS10" } },
            { "SMCR",   { "", "", "", "", "SE", "SM2", "SM1", "SM0" } },
            { "WDTCR",  { "", "", "", "WDCE", "WDE", "WDP2", "WDP1", "WDP0" } },
            { "ASSR",   { "", "", "", "", "AS2", "TCN2UB", "OCR2UB", "TCR2UB" } }
        };
        const char* bl[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        for (auto& row : bt) {
            if (lower(row.nm) == lower(rn)) { for (int i = 0; i < 8; i++) bl[i] = row.bl[i]; break; }
        }
        int named = 0;
        for (int i = 0; i < 8; i++) if (bl[i] && bl[i][0]) named++;
        if (named >= 2) {
            std::string l1 = "  ", l2 = "  ";
            for (int i = 0; i < 8; i++) {
                if (bl[i] && bl[i][0]) l1 += fmt("%-6s", bl[i]);
                else                   l1 += fmt("%-6s", fmt("b%d", 7 - i).c_str());
                l2 += fmt("%-6s", fmt("%d", (v >> (7 - i)) & 1).c_str());
            }
            wr(l1, DGRAY);
            wr(l2, GRAY);
        } else {
            std::string l = "  установлены биты:";
            bool any = false;
            for (int i = 0; i < 8; i++) if ((v >> (7 - i)) & 1) { l += fmt(" %d", 7 - i); any = true; }
            wr(any ? l : "  все нули", GRAY);
        }
        return true;
    }
    if (cmd == "uart" || cmd == "tio") {
        // Terminal I/O (как окно Terminal I/O в AVR Studio)
        std::string s1 = lower(a(1));
        if (g_udr < 0 && (s1 == "push" || s1 == "send")) { wr(" В профиле нет USART (UDR) — Terminal I/O недоступен", RED); return true; }
        if (s1 == "push" || s1 == "send") {
            std::string txt;
            for (int q = 2; !a(q).empty(); q++) { if (!txt.empty()) txt += ' '; txt += a(q); }
            if (txt.empty()) { wr(" формат: uart push <текст> — приём байтов (Terminal I/O)", RED); return true; }
            for (size_t q = 0; q < txt.size(); q++) {
                if (q == 0) g_dmem[g_udr] = (uint8_t)txt[0];
                else        g_tioRx.push_back((uint8_t)txt[q]);
            }
            if (g_ucsrA >= 0) g_dmem[g_ucsrA] |= 0x80;          // RXC
            wr(fmt(" RX: %u байт(а) в очередь USART (UDR + RXC)", (unsigned)txt.size()), GREEN);
            return true;
        }
        if (s1 == "clear") { g_tioTx.clear(); wr(" TX-лог очищен", GREEN); return true; }
        wr(" Terminal I/O (USART):", CYAN);
        if (g_tioTx.empty()) wr("  TX: (пусто — записи в UDR попадают сюда)");
        else {
            for (size_t off = 0; off < g_tioTx.size(); off += 64) {
                std::string l = "  TX: ";
                size_t e = (std::min)(off + 64, g_tioTx.size());
                for (size_t q = off; q < e; q++) {
                    unsigned char c = (unsigned char)g_tioTx[q];
                    l += (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                }
                wr(l);
            }
        }
        if (g_udr < 0) {
            wr("  RX: USART отсутствует в профиле", DGRAY);
            return true;
        }
        wr(fmt("  RX: в UDR=0x%02X RXC=%d, в очереди %u",
               g_dmem[g_udr], (readDmem(g_ucsrA) >> 7) & 1, (unsigned)g_tioRx.size()), DGRAY);
        return true;
    }
    if (cmd == "iotree" || cmd == "ot") {
        // I/O window: регистры профиля, сгруппированные по модулям периферии
        struct Grp { const char* nm; const char* pfx[9]; };
        static const Grp G[8] = {
            { "GPIO",    { "ddr", "port", "pin", "pinctrl", "vport", "gpio", "tcb_", 0, 0 } },
            { "TIMER",   { "tcnt", "tccr", "tifr", "timsk", "ocr", "icr", "dtimsk", "hires", "awex" } },
            { "USART",   { "usart", "ucs", "ubrr", "udr", 0, 0, 0, 0, 0 } },
            { "SPI",     { "spc", "sps", "spd", "spis", 0, 0, 0, 0, 0 } },
            { "ADC",     { "adc", "admux", "didr", 0, 0, 0, 0, 0, 0 } },
            { "AC",      { "acs", "acsr", 0, 0, 0, 0, 0, 0, 0 } },
            { "TWI",     { "tw", 0, 0, 0, 0, 0, 0, 0, 0 } },
            { "EEPROM",  { "ee", 0, 0, 0, 0, 0, 0, 0, 0 } }
        };
        std::vector<std::string> bks[9];
        for (auto& kv : g_regmap) {
            std::string n = kv.first;
            int g = -1;
            // CPU-блок: точные имена + WDT/GPIOR
            if (n == "sreg" || n == "sph" || n == "spl" || n == "spmcr" || n == "spmcsr" ||
                n == "smcr" || n == "mcucr" || n == "mcucsr" || n == "mcusr" ||
                n.compare(0, 3, "wdt") == 0 || n.compare(0, 5, "gpior") == 0) g = 8;
            else {
                for (int gi = 0; gi < 8 && g < 0; gi++)
                    for (int q = 0; q < 9 && g < 0; q++) {
                        const char* px = G[gi].pfx[q];
                        if (!px) break;
                        if (n.size() >= strlen(px) && n.compare(0, strlen(px), px) == 0) g = gi;
                    }
            }
            bks[g >= 0 ? g : 8].push_back(n);
        }
        std::string flt = lower(a(1));
        wr(" I/O window (карта профиля):", CYAN);
        for (int gi = 0; gi < 9; gi++) {
            std::string gnm = (gi < 8) ? G[gi].nm : "CPU/WDT/прочее";
            if (bks[gi].empty()) continue;
            if (flt.empty()) wr(fmt(" [%s]", gnm.c_str()), YELLOW);
            for (auto& n : bks[gi]) {
                if (!flt.empty() && n.find(flt) == std::string::npos) continue;
                int s = g_regmap[n];
                if (s - 0x20 >= 0 && s - 0x20 <= 0x3F)
                    wr(fmt("   %-14s IO 0x%02X  0x%02X", upper(n).c_str(), s - 0x20, readDmem(s)));
                else
                    wr(fmt("   %-14s SR 0x%04X 0x%02X", upper(n).c_str(), s, readDmem(s)));
            }
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
            tickInts(cy);
            dispatchPending();
            renderDash();
            Sleep(dms);
        }
        return true;
    }
    if (cmd == "quit" || cmd == "exit" || cmd == "q") return false;
    if (cmd == "selftest") { runSelfTest(); return true; }
    if (cmd == "selftest2" || cmd == "st2") { runSelfTest2(); return true; }
    if (cmd == "quiet") { g_quiet = true; return true; }
    if (cmd == "isr" || cmd == "iv") {
        // interrupt dispatcher status (registers taken from profile [RegisterMap])
        wr(fmt(" I=%d (SREG) | vectors in profile: %d", g_f.I, (int)g_isrv.size()), CYAN);
        bool hasEimsk = g_regmap.count("eimsk") > 0;
        bool hasTf0   = g_regmap.count("tifr0") > 0 && g_regmap.count("tifr1") == 0;
        int iscIO  = (hasEimsk && g_regmap.count("eicra")) ? g_regmap["eicra"] : 0x55;
        int flagIO = hasEimsk ? ((g_regmap.count("eifr"))  ? g_regmap["eifr"] : 0x3C) : 0x5A;
        int enIO   = hasEimsk ? ((g_regmap.count("eimsk")) ? g_regmap["eimsk"] : 0x3D)
                              : (hasTf0 ? ((g_regmap.count("gimsk")) ? g_regmap["gimsk"] : 0x5B)
                                        : ((g_regmap.count("gicr"))    ? g_regmap["gicr"] : 0x5B));
        uint8_t isc = readDmem(iscIO);
        static const char* modeN[4] = { "low-level", "any change", "falling edge", "rising edge" };
        struct X { int pinIO, bit, flagBit, enBit, iscM, vec; const char* nm; };
        X e[2] = { { g_int0PinIO, g_int0Bit, 6, 6, 0, -1, "INT0_vect" },
                   { g_int1PinIO, g_int1Bit, 7, 7, 2, -1, "INT1_vect" } };
        if (hasEimsk) { e[0].flagBit = 0; e[0].enBit = 0; e[1].flagBit = 1; e[1].enBit = 1; }
        for (int i = 0; i < 2; i++) {
            auto vi = g_isrv.find(i == 0 ? "int0_vect" : "int1_vect");
            if (vi == g_isrv.end()) continue;
            e[i].vec = vi->second;
            if (i == 1 && hasTf0) continue;
            char pch = '?';
            for (int k = 0; k < (int)g_pio.size(); k++)
                if (g_pio[k].pin == e[i].pinIO) pch = (char)toupper((unsigned char)g_pname[k][0]);
            uint8_t lvl = (uint8_t)((readDmem(0x20 + e[i].pinIO) >> e[i].bit) & 1);
            int mode = (isc >> e[i].iscM) & 3;
            wr(fmt("  %s pin=P%c%d level=%d <%s> en=%d flag=%d vec=#%d",
                   e[i].nm, pch, e[i].bit, lvl, modeN[mode],
                   (readDmem(enIO) >> e[i].enBit) & 1,
                   (readDmem(flagIO) >> e[i].flagBit) & 1, e[i].vec), CYAN);
        }
        static const int base[8] = { 0, 1, 8, 64, 256, 1024, 0, 0 };    // T0/T1: CS 6..7 = ext pin (не эмулируется)
        static const int t2tb[8] = { 0, 1, 8, 32, 64, 128, 256, 1024 }; // T2
        for (int t = 0; t < 3; t++) {
            if (!g_ts[t].have) continue;
            int cs = readDmem(g_ts[t].tccr) & 7;
            int n = (t == 2) ? t2tb[cs] : base[cs];
            const char* src = n > 1 ? faPres(n) : ((t != 2 && cs >= 6) ? "ext pin (не тикает)" : "stopped");
            uint16_t tval = (uint16_t)(readDmem(g_ts[t].tcntL) & 0xFF);
            if (g_ts[t].tcntH >= 0) tval = (uint16_t)(tval | ((readDmem(g_ts[t].tcntH) & 0xFF) << 8));
            wr(fmt("  T%d TCCR CS=%d (%s) TCNT=0x%04X en=%d flag=%d",
                   t, cs, src, tval,
                   (readDmem(g_ts[t].timsk) >> g_ts[t].toieBit) & 1,
                   (readDmem(g_ts[t].tifr)  >> g_ts[t].tovBit) & 1), CYAN);
        }
        // vector table (index order)
        std::vector<std::pair<int, std::string>> vt;
        for (auto& kv : g_isrv) vt.push_back({ kv.second, upper(kv.first) });
        std::sort(vt.begin(), vt.end());
        for (auto& v : vt) wr(fmt("   #%d  %-17s @PC=0x%04X", v.first, v.second.c_str(), v.first), DGRAY);
        return true;
    }
    if (cmd == "help" || cmd == "?") {
        wr("Commands (README sec.12 + ext):", CYAN);
        wr("  load <elf> | reset(rst)            - load/reset program");
        wr("  run(r) [maxcyc] | auto(au) [period]");
        wr("  stepi(si) [n] | step(s) [n] | next(n) [n]   - step instructions / over calls");
        wr("  finish(fin) | until(u) <addr|sym>  - run-to-return / run-to-target");
        wr("  reg R# [= val] | sreg | all        - read/assign registers");
        wr("  io <REG>                           - bit-level view of the register (io TIMSK)");
        wr("  regs [filter]                      - register map of the profile [RegisterMap]");
        wr("  stk (stack)                        - stack content from SP upward");
        wr("  port <A|B|C|D>                     - port pins report (IN/OUT/PullUp)");
        wr("  pin <P#|P#bit> <v|toggle>          - stimulus");
        wr("  mem sram|flash <addr> [len]        - dump; write: mem sram 0x60 = 0xFF");
        wr("  bp add|ba <addr|sym|vect>  list|bl  del|bd <n|all>");
        wr("  isr (int status)   w <var|addr> (watch)   tr (timer reset)   sz size   da (disasm)");
        wr("  iotree (ot) [filter] - window I/O by modules;  uart push|clear (tio) - \"screen\" Terminal I/O");
        wr("  selftest | selftest2 | quiet | quit(q)");
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
    wr("  Type 'help' for commands.                                      (emu.cpp v1.0)", DCYAN);
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
