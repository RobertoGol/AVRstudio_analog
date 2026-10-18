// AVR CLI Studio - GUI (Win32, чистый нативный API; портативный один exe)
// Стиль и расположение копируют AVR Studio 4 по скринов Pictures/1..25.png.
// Этап M1 (каркас): главное окно, меню AS4, статусбар, Welcome-диалог,
// правый док I/O View (модули -> регистры -> биты), нижний Message с табами.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>
#include <map>
#include <vector>
#include <atomic>

#include "emu_api.h"   // ядро эмулятора встроено в процесс (build/emu.cpp, EMU_LIB)

// перехватчик необработанных исключений (пишет код/адрес в avrgui.log)
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("G:\\Windows\\RJ\\AppData\\Local\\Temp\\opencode\\avrgui.log", "a");
    if (f) { fprintf(f, "EXCEPTION code=0x%08X addr=%p\n", (unsigned)ep->ExceptionRecord->ExceptionCode,
                     ep->ExceptionRecord->ExceptionAddress); fclose(f); }
    return EXCEPTION_EXECUTE_HANDLER;
}


#pragma comment(lib, "comctl32.lib")   // для MSVC-compatible; mingw линкует через -l

#define IDM_MENU_BASE   100

// --- ID команд меню (названия взяты 1:1 из скринов 8..10) ---
enum {
    // File
    IDM_FILE_NEW = 1000, IDM_FILE_OPEN, IDM_FILE_CLOSE, IDM_FILE_SAVE, IDM_FILE_SAVEAS,
    IDM_FILE_SAVEALL, IDM_FILE_PRINT, IDM_FILE_PRINTSETUP, IDM_FILE_EXIT,
    // Project
    IDM_PROJ_WIZARD = 1100, IDM_PROJ_NEW, IDM_PROJ_OPEN, IDM_PROJ_SAVE, IDM_PROJ_CLOSE,
    IDM_PROJ_RECENT,
    // Build
    IDM_BUILD_COMPILE = 1200, IDM_BUILD_RUN, IDM_BUILD_CLEAN, IDM_BUILD_OPTIONS,
    IDM_BUILD_BUILD,
    // View
    IDM_VIEW_STD = 1300, IDM_VIEW_EDITBAR, IDM_VIEW_BUILDBAR, IDM_VIEW_DEBUGWIN,
    IDM_VIEW_MDI, IDM_VIEW_STK, IDM_VIEW_TRACE,
    IDM_VIEW_STATUS, IDM_VIEW_DISASM, IDM_VIEW_WATCH, IDM_VIEW_MEM, IDM_VIEW_MEM2,
    IDM_VIEW_MEM3, IDM_VIEW_REG, IDM_VIEW_IO, IDM_VIEW_PROC,
    IDM_VIEW_BUILDOUT, IDM_VIEW_MSGOUT, IDM_VIEW_FINDOUT, IDM_VIEW_BREAKS,
    // Edit (скрин 9/12: Undo/Redo/Cut/Copy/Paste/Delete/SelectAll/Bookmarks)
    IDM_EDIT_UNDO = 1420, IDM_EDIT_REDO, IDM_EDIT_CUT, IDM_EDIT_COPY, IDM_EDIT_PASTE,
    IDM_EDIT_DELETE, IDM_EDIT_SELALL, IDM_EDIT_BMTGGL, IDM_EDIT_BMPREV, IDM_EDIT_BMNEXT,
    IDM_EDIT_BMRMALL,
    // Window (каскад/тайлинг/список окон)
    IDM_WIN_CASCADE = 1440, IDM_WIN_TILEH, IDM_WIN_TILEV, IDM_WIN_ARRANGE, IDM_WIN_LIST,
    // Tools
    IDM_TOOLS_AVRPROG = 1400, IDM_TOOLS_CUSTOM, IDM_TOOLS_OPTIONS, IDM_TOOLS_KEYS,
    IDM_TOOLS_PLUGIN, IDM_TOOLS_PROG_CONNECT, IDM_TOOLS_PROGP_AUTO, IDM_TOOLS_PROGP_FLASH,
    IDM_TOOLS_PROGP_EEWRITE, IDM_TOOLS_PROGP_EREAD, IDM_TOOLS_PROGP_AUTOSTART,
    IDM_TOOLS_FLIP, IDM_TOOLS_WLESS, IDM_TOOLS_BATT, IDM_TOOLS_QTOUCH, IDM_TOOLS_AVR32,
    // Debug
    IDM_DBG_START = 1500, IDM_DBG_STOP, IDM_DBG_RUN, IDM_DBG_BREAK, IDM_DBG_RESET,
    IDM_DBG_STEPIN, IDM_DBG_STEPOVER, IDM_DBG_STEPOUT, IDM_DBG_AUTOSTEP,
    IDM_DBG_RUNTOCUR, IDM_DBG_NEXTBP, IDM_DBG_REMOVEBP,
    IDM_DBG_TRACE, IDM_DBG_STACKMON, IDM_DBG_SHOWNEXT, IDM_DBG_QUICKWATCH,
    IDM_DBG_PLATFORM, IDM_DBG_MEMUPDOWN,
    // Help
    IDM_HELP_TOOLSUG = 1600, IDM_HELP_USERSUG, IDM_HELP_UPGRADE, IDM_HELP_RELNOTES,
    IDM_HELP_ABOUT
};

static HWND g_main = 0, g_ioTree = 0, g_ioList = 0, g_msgs = 0, g_status = 0;
static HWND g_tbStd = 0, g_tbDbg = 0, g_editMain = 0;
// Dock Workspace слева (скрин 7) и нижняя зона вывода со вкладками снизу (скрин 15)
static HWND g_wsCap = 0, g_wsTree = 0, g_wsTabs = 0, g_wsList = 0;
static HWND g_outCap = 0, g_outTabs = 0, g_buildEdit = 0, g_findEdit = 0, g_bpList = 0;
static std::vector<std::string> g_wsFiles;       // пути файлов дерева Workspace
static int g_outPage = 1;                        // активная страница вывода: 0 Build 1 Message 2 Find 3 BP
static HFONT g_fontUi = 0;

// ------------------------------------------------------------ Caption-панель дока
// Синяя строка заголовка дока как в AS4 (скрин 15): слева имя, справа ▼ и ✕
static LRESULT CALLBACK CapProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r; GetClientRect(h, &r);
            HBRUSH blue = CreateSolidBrush(RGB(0, 102, 204));
            FillRect(dc, &r, blue);
            DeleteObject(blue);
            RECT leftCol = { r.left, r.top, r.left + 4, r.bottom };   // тёмный якорь слева, как у AS4
            HBRUSH dark = CreateSolidBrush(RGB(0, 64, 143));
            FillRect(dc, &leftCol, dark);
            DeleteObject(dark);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            SelectObject(dc, (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            char text[128]; GetWindowTextA(h, text, sizeof(text));
            RECT tr = { r.left + 8, r.top, r.right - 44, r.bottom };
            DrawTextA(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            // справа ▼ и ✕ (рисуем сами, ANSI DrawText кириллические glyфы не даст)
            POINT tri[3] = { {r.right-34, r.top+8}, {r.right-24, r.top+8}, {r.right-29, r.top+14} };
            HBRUSH wb = CreateSolidBrush(RGB(255,255,255));
            HPEN noPen = (HPEN)GetStockObject(NULL_PEN);
            HGDIOBJ op = SelectObject(dc, noPen), ob = SelectObject(dc, wb);
            Polygon(dc, tri, 3);
            SelectObject(dc, op); SelectObject(dc, ob);
            DeleteObject(wb);
            HPEN wp = CreatePen(PS_SOLID, 1, RGB(255,255,255));
            HGDIOBJ open_ = SelectObject(dc, wp);
            int cxm = r.right - 14, cym = r.top + 10;
            MoveToEx(dc, cxm - 4, cym - 4, 0); LineTo(dc, cxm + 4, cym + 4);
            MoveToEx(dc, cxm + 4, cym - 4, 0); LineTo(dc, cxm - 4, cym + 4);
            SelectObject(dc, open_);
            DeleteObject(wp);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcA(h, m, w, l);
}

static void registerCapClass() {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = CapProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AVRCLI_Cap";
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
}

static void tbAddStr(HWND tb, int id, const char* text, int sep) {    TBBUTTON b = {0};
    b.iBitmap = 0;
    b.idCommand = id;
    b.fsState = TBSTATE_ENABLED;
    b.fsStyle = (BYTE)(sep ? BTNS_SEP : BTNS_SHOWTEXT | BTNS_AUTOSIZE);
    b.iString = (INT_PTR)text;
    SendMessageA(tb, TB_ADDBUTTONS, 1, (LPARAM)&b);
}

// ---------------------------------------------------------------- I/O View
// Модули и регистры AVR Studio 'part description' - сейчас зеркалим ATmega8
// (скрины 7/18-25); позже заполняется из [RegisterMap] профиля чипа.
struct IoReg { const char* name; int ioAddr; };
struct IoModule { const char* name; const IoReg* regs; int nRegs; };

static const IoReg kCpuRegs[] = {
    {"MCUCR", 0x35}, {"MCUCSR", 0x34}, {"OSCCAL", 0x31}, {"SFIOR", 0x30},
    {"SP", 0x3D}, {"SPH", 0x3E}, {"SPL", 0x3D}, {"SPMCR", 0x37}, {"SREG", 0x3F},
};
static const IoModule kModules[] = {
    {"AD_CONVERTER", 0, 0},
    {"ANALOG_COMPARATOR", 0, 0},
    {"CPU", kCpuRegs, 9},
    {"EEPROM", 0, 0},
    {"EXTERNAL_INTERRUPT", 0, 0},
    {"PORTB", 0, 0}, {"PORTC", 0, 0}, {"PORTD", 0, 0},
    {"SPI", 0, 0},
    {"TIMER_COUNTER_0", 0, 0}, {"TIMER_COUNTER_1", 0, 0}, {"TIMER_COUNTER_2", 0, 0},
    {"TWI", 0, 0}, {"USART", 0, 0}, {"WATCHDOG", 0, 0},
};

static WNDPROC g_treeProc = 0;
static LRESULT CALLBACK TreeSubclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    return CallWindowProc(g_treeProc, h, m, w, l);
}

static void addTreeItem(HWND tv, HTREEITEM parent, const char* text, int image) {
    TVINSERTSTRUCTA tvi = {0};
    tvi.hParent = parent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    tvi.item.pszText = (LPSTR)text;
    tvi.item.iImage = image;
    tvi.item.iSelectedImage = image;
    TreeView_InsertItem(tv, &tvi);
}

static void fillIoTree(HWND tv) {
    TreeView_DeleteAllItems(tv);
    int nMod = sizeof(kModules)/sizeof(kModules[0]);
    for (int m = 0; m < nMod; m++) {
        // root-модуль (скрин 20: серая иконка藏族 + имя)
        TVINSERTSTRUCTA tvi = {0};
        tvi.hInsertAfter = TVI_LAST;
        tvi.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        tvi.item.pszText = (LPSTR)kModules[m].name;
        tvi.item.iImage = 0; tvi.item.iSelectedImage = 0;
        HTREEITEM hMod = TreeView_InsertItem(tv, &tvi);
        // children-регистры по скрину 21 (только CPU заполнен пока)
        if (kModules[m].regs) {
            for (int r = 0; r < kModules[m].nRegs; r++)
                addTreeItem(tv, hMod, kModules[m].regs[r].name, 1);
        }
    }
    TreeView_Expand(tv, TreeView_GetRoot(tv), TVE_EXPAND);
}

// ------------------------------------------------------------ Message panel
static void msgAppend(const char* line) {
    static int winLen = 4096;
    int n = GetWindowTextLengthA(g_msgs);
    if (n < 0) n = 0;
    std::string cur(n, 0);
    if (n > 0) {
        // WM_SETTEXT работает с read-only окном без фокуса, в отличие от EM_REPLACESEL
        SendMessageA(g_msgs, WM_GETTEXT, n + 1, (LPARAM)&cur[0]);
        if (!cur.empty() && cur.back() != '\n') cur += "\r\n";
    }
    cur += line;
    SendMessageA(g_msgs, WM_SETTEXT, 0, (LPARAM)cur.c_str());
    (void)winLen;
}

// ------------------------------------------------------------- Ядро эмулятора
static bool g_coreInit = false;                    // emuapi_init выполнен
static std::atomic<bool> g_runBusy(false);         // работает worker-поток прогона
static std::string g_projRoot;                     // корень проекта (echo.ctos + исходники)
static std::string g_appDir;                       // папка exe (для recent.txt, шаблонов, Procs)
static std::string g_deviceName = "ATmega8A";      // ProcessorProfile открытого проекта
static std::string g_mcuName    = "atmega8";       // MCU_Targetффективный (для -mmcu)
static char g_valText[16];                         // буфер строк для ListView (должен жить)

static void uiAppend(const char* s) {              // вывод ядра -> Message-панель
    msgAppend(s);
}

static void debugStateLine() {
    static char line[128];
    snprintf(line, sizeof(line), "PC=0x%04X  SP=0x%04X  SREG=0x%02X  cycle=%llu",
             emuapi_pc(), emuapi_sp(), emuapi_sreg(),
             (unsigned long long)emuapi_cycles());
    msgAppend(line);
}

// обновить правый I/O View значениями ядра (строки листа CPU-модуля)
static void refreshIoListView() {
    if (!g_coreInit) return;
    for (int r = 0; r < 9; r++) {
        const IoReg& ir = kCpuRegs[r];
        uint8_t v = emuapi_readSram(0x20 + ir.ioAddr);
        snprintf(g_valText, sizeof(g_valText), "0x%02X", v);
        LVITEMA it = {0};
        it.mask = LVIF_TEXT; it.iItem = r; it.iSubItem = 2; it.pszText = g_valText;
        SendMessageA(g_ioList, LVM_SETITEMA, 0, (LPARAM)&it);
    }
    debugStateLine();
}

// ------------------------------------------------------------ Processor (скрин 16)
// Отдельное окно ListView Name|Value: PC/SP/X/Y/Z/Cycle/Freq/StopWatch/SREG + R00..R31
static HWND g_procWin = nullptr, g_procList = nullptr;
static bool g_procItems = false;      // имена строк уже заполнены

static void procSetItem(int idx, const char* text) {
    LVITEMA it = {0};
    it.mask = LVIF_TEXT; it.iItem = idx; it.iSubItem = 1; it.pszText = (LPSTR)text;
    if (g_procList) SendMessageA(g_procList, LVM_SETITEMTEXTA, idx, (LPARAM)&it);
    if (g_wsList && IsWindowVisible(g_wsList)) SendMessageA(g_wsList, LVM_SETITEMTEXTA, idx, (LPARAM)&it);
}

static LRESULT CALLBACK ProcWinProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (g_procList) MoveWindow(g_procList, 0, 0, LOWORD(l), HIWORD(l), TRUE);
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE); return 0;   // как в AS4: крестик прячет окно, доступ вернёт View > Processor
        case WM_DESTROY:
            g_procWin = g_procList = nullptr; g_procItems = false;
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void refreshProcWin() {
    if (!g_coreInit) return;
    bool wsListVis = g_wsList && IsWindowVisible(g_wsList);
    if (!g_procWin && !wsListVis) return;
    char t[64];
    // X = R26:R27, Y = R28:R29, Z = R30:R31 (регистры_file лежит в SRAM 0..31)
    uint16_t xv = (uint16_t)(emuapi_readSram(26) | (emuapi_readSram(27) << 8));
    uint16_t yv = (uint16_t)(emuapi_readSram(28) | (emuapi_readSram(29) << 8));
    uint16_t zv = (uint16_t)(emuapi_readSram(30) | (emuapi_readSram(31) << 8));
    unsigned fcpu = emuapi_fcpu();
    unsigned long long cyc = emuapi_cycles();
    snprintf(t, sizeof(t), "0x%04X", (unsigned)emuapi_pc());
    procSetItem(0, t);
    snprintf(t, sizeof(t), "0x%04X", emuapi_sp());      procSetItem(1, t);
    snprintf(t, sizeof(t), "0x%04X", xv);               procSetItem(2, t);
    snprintf(t, sizeof(t), "0x%04X", yv);               procSetItem(3, t);
    snprintf(t, sizeof(t), "0x%04X", zv);               procSetItem(4, t);
    snprintf(t, sizeof(t), "%llu", cyc);                procSetItem(5, t);
    snprintf(t, sizeof(t), "%.3f MHz", fcpu / 1000000.0); procSetItem(6, t);
    snprintf(t, sizeof(t), "%.3f ms", cyc * 1000.0 / (fcpu ? fcpu : 1)); procSetItem(7, t);
    snprintf(t, sizeof(t), "0x%02X", emuapi_sreg());    procSetItem(8, t);
    for (unsigned r = 0; r < 32; r++) {
        snprintf(t, sizeof(t), "0x%02X", emuapi_readSram((int)r));
        procSetItem(10 + (int)r, t);
    }
}

static void addProcRowsOnce(HWND list) {
    static const char* kNames[10] = {
        "Program Counter", "Stack Pointer", "X pointer", "Y pointer", "Z pointer",
        "Cycle Counter", "Frequency", "Stop Watch", "SREG", "Registers"
    };
    char n[8];
    for (int i = 0; i < 10; i++) {
        LVITEMA it = {0};
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (LPSTR)kNames[i];
        SendMessageA(list, LVM_INSERTITEMA, i, (LPARAM)&it);
    }
    for (unsigned r = 0; r < 32; r++) {
        snprintf(n, sizeof(n), "R%02u", r);
        LVITEMA it = {0};
        it.mask = LVIF_TEXT; it.iItem = 10 + (int)r; it.pszText = n;
        SendMessageA(list, LVM_INSERTITEMA, 10 + (int)r, (LPARAM)&it);
    }
}

// Показ окна Processor (View > Processor). Возвращает true, если окно открыто/стало видимым.
static bool showProcWin(HWND parent) {
    if (!parent) parent = g_main;
    if (g_procWin) { ShowWindow(g_procWin, SW_SHOW); SetForegroundWindow(g_procWin); refreshProcWin(); return true; }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = ProcWinProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AVRCLI_ProcWin";
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    g_procWin = CreateWindowExA(WS_EX_TOOLWINDOW, "AVRCLI_ProcWin", "Processor",
        WS_POPUP | WS_CAPTION | WS_SIZEBOX | WS_SYSMENU, 800, 200, 320, 560,
        parent, nullptr, GetModuleHandleA(nullptr), nullptr);
    g_procList = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                 0, 0, 316, 530, g_procWin, (HMENU)(intptr_t)1, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(g_procList, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    LVCOLUMNA col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 150; col.pszText = (LPSTR)"Name";
    SendMessageA(g_procList, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = (LPSTR)"Value";
    SendMessageA(g_procList, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    SendMessageA(g_procList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    addProcRowsOnce(g_procList);
    g_procItems = true;
    ShowWindow(g_procWin, SW_SHOW);
    refreshProcWin();
    return true;
}

// worker-поток прогона (F5): завершение уведомляет главное окно
static DWORD WINAPI runThreadProc(LPVOID) {    emuapi_run();
    g_runBusy = false;
    if (g_main) PostMessageA(g_main, WM_APP + 1, 0, 0);
    return 0;
}

// старт отладки: инициализация ядра + загрузка ELF открытого проекта
static void startDebugging() {
    if (g_runBusy) { msgAppend("-- Прогон уже работает (Break = Ctrl+F5)"); return; }
    if (g_projRoot.empty()) { msgAppend("! Нет открытого проекта. Project > New/Open Project."); return; }
    if (!g_coreInit) {
        // Procs-каталог живёт рядом с exe (общий каталог чипов на 207 профилей)
        emuapi_setProcsDir((g_appDir + "/../Procs").c_str());
        emuapi_sink(uiAppend);
        emuapi_init(g_projRoot.c_str(), (g_projRoot + "/main.elf").c_str());
        g_coreInit = true;
        msgAppend((std::string("Core: chip=") + emuapi_chip() +
                   "  F_CPU=" + std::to_string(emuapi_fcpu()) + " Hz").c_str());
    }
    if (!emuapi_loaded() && !emuapi_load()) return;
    msgAppend("-- Debugging started (SIMULATOR)");
}

// ------------------------------------------------------------------ Status
static void statusSet(int part, const char* txt) {
    SendMessageA(g_status, SB_SETTEXTA, part | SBT_NOBORDERS, (LPARAM)txt);
}

// --------------------------------------------------------------- Диалоги из кода
// AVR Studio 4: Welcome (скрин 1) и мастер (скрины 2,5,6). .rc-ресурсы не нужны —
// строю обычные popup-окна с дочерними контролами (DLGTEMPLATE в памяти слишком
// хрупок: ANSI-раскладка строк и выравнивания легко съезжают).

// ------------------------------------------------------------- Проект
struct Recent { std::string name, path; };
static std::vector<Recent> g_recent;
static std::string g_srcPath;          // главный исходник проекта (полный путь)
static bool g_projOpen = false;

static std::string exeDirNow() {
    char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p; size_t sl = s.find_last_of("\\/"); 
    return sl != std::string::npos ? s.substr(0, sl) : std::string(".");
}

static bool fileExists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// read echo.ctos в GUI (семантика readPairs ядра: UTF-8 без BOM, key = value, секции пропускаются)
static std::map<std::string, std::string> readCtos(const std::string& dir) {
    std::map<std::string, std::string> out;
    FILE* f = fopen((dir + "/echo.ctos").c_str(), "rb");
    if (!f) return out;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        if (!s.empty() && s.back() == '\n') s.pop_back();
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (s.empty() || s[0] == '[' || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        auto trim = [](std::string t) { 
            size_t a = t.find_first_not_of(" \t"); 
            if (a == std::string::npos) return std::string();
            size_t b = t.find_last_not_of(" \t");
            return t.substr(a, b - a + 1);
        };
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        out[trim(k)] = trim(v);
    }
    fclose(f);
    return out;
}

static void writeTextFile(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { msgAppend((std::string("! Не создать файл: ") + path).c_str()); return; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

static std::string readTextFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static std::string echoTemplate(const char* device) {
    // MCU_Target: маленькими буквами для -mmcu (avr-gcc 4.5.1 знает atmega8/8a)
    std::string mcuL = device; for (auto& c : mcuL) c = (char)tolower((unsigned char)c);
    std::string devU = device; for (auto& c : devU) c = (char)toupper((unsigned char)c);
    std::string o;
    o += "# AVR CLI Studio project state (UTF-8, no BOM)\n";
    o += "[ActiveSession]\n";
    o += "ProcessorProfile = " + std::string(device) + "\n";
    o += "MCU_Target = " + mcuL + "\n";
    o += "Define_Tag = __AVR_" + devU + "__\n";
    o += "F_CPU = 8000000UL\n";
    o += "Optimization = O0\n\n";
    o += "[Paths]\n";
    o += "ToolchainBin = C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/bin\n";
    o += "ProcsDir = ./Procs\n";
    o += "[EmulatorState]\n";
    o += "LastPC = 0x0000\n";
    return o;
}

static const char* kBlinkTemplate =
"#include <avr/io.h>\r\n"
"#include <util/delay.h>\r\n"
"\r\n"
"int main(void)\r\n"
"{\r\n"
"    DDRB = 0xFF;              /* PB0..PB7 as output */\r\n"
"    for (;;) {\r\n"
"        PORTB ^= 0x01;        /* blink PB0 */\r\n"
"        _delay_ms(500);\r\n"
"    }\r\n"
"}\r\n";

static void loadEditorFromFile(const std::string& path) {
    std::string txt = readTextFile(path);
    for (size_t i = 0; i + 1 < txt.size() && txt[i] == 0xEF && txt[i+1] == 0xBB && txt[i+2] == 0xBF; ) 
        txt.erase(0, 3);                       // снять BOM
    // CRLF-фикс для EDIT
    if (txt.find("\r\n") == std::string::npos) {
        std::string out; out.reserve(txt.size() + 64);
        for (char c : txt) { if (c == '\n') out += "\r\n"; else out += c; }
        txt = out;
    }
    SetWindowTextA(g_editMain, txt.c_str());
    g_srcPath = path;
}

static void saveEditor() {
    if (g_srcPath.empty()) { msgAppend("! Нет файла для сохранения"); return; }
    int n = GetWindowTextLengthA(g_editMain);
    if (n < 0) n = 0;
    // EDIT отдаёт текст с \r\n - в файл пишем как есть (норм для gcc)
    std::string txt(n, 0);
    GetWindowTextA(g_editMain, &txt[0], n + 1);
    writeTextFile(g_srcPath, txt);
    msgAppend(("Saved: " + g_srcPath).c_str());
}

// перечень чипов из каталога профилей (для мастера - скрин 5/6, список устройств)
static std::vector<std::string> listDevices(const std::string& procsDir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((procsDir + "/*.proc").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        size_t dot = n.rfind(".proc");
        if (dot != std::string::npos) n = n.substr(0, dot);
        out.push_back(n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

static void addRecent(const std::string& name, const std::string& path) {
    for (size_t i = 0; i < g_recent.size(); i++)
        if (g_recent[i].path == path) { g_recent.erase(g_recent.begin() + i); break; }
    Recent r; r.name = name; r.path = path;
    g_recent.insert(g_recent.begin(), r);
    if (g_recent.size() > 9) g_recent.resize(9);
    // сохранить
    std::string s;
    for (auto& r : g_recent) s += r.name + "|" + r.path + "\n";
    writeTextFile(g_appDir + "/recent.txt", s);
}

static void loadRecent() {
    std::string s = readTextFile(g_appDir + "/recent.txt");
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
        std::string ln = s.substr(p, e - p); p = e + 1;
        size_t bar = ln.find('|');
        if (bar != std::string::npos) {
            Recent r; r.name = ln.substr(0, bar); r.path = ln.substr(bar + 1);
            g_recent.push_back(r);
        }
    }
}

// открыть проект: dir с echo.ctos
static void fillWorkspaceTree();          // дерево проекта слева (скрин 7), реализация ниже
static void addProcRowsOnce(HWND list);   // строки Processor-таблицы (скрин 16)

static void openProjectAt(const std::string& projDir) {
    auto cfg = readCtos(projDir);
    g_projRoot = projDir;
    g_deviceName = cfg.count("processorprofile") ? cfg["processorprofile"] : "ATmega8A";
    g_mcuName    = cfg.count("mcu_target") ? cfg["mcu_target"] : std::string();
    if (g_mcuName.empty()) { g_mcuName = g_deviceName; for (auto& c : g_mcuName) c = (char)tolower((unsigned char)c); }
    g_projOpen = true;
    g_coreInit = false;        // следующий Start Debugging перечитает окружение
    std::string src = readTextFile(projDir + "/main.c").size() ? projDir + "/main.c" : projDir + "/main.asm";
    if (fileExists(src)) loadEditorFromFile(src);
    else { g_srcPath = projDir + "/main.c"; SetWindowTextA(g_editMain, "\r\n"); }
    statusSet(0, g_deviceName.c_str());
    msgAppend((std::string("Project opened: ") + projDir).c_str());
    msgAppend((std::string("Device: " + g_deviceName + "  (mcu=" + g_mcuName + ")").c_str()));
    fillWorkspaceTree();
}

static std::string folderNameOf(const std::string& p) {
    std::string s = p;
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    size_t sl = s.find_last_of("\\/");
    return sl != std::string::npos ? s.substr(sl + 1) : s;
}

// ------------------------------------------------------------ Dock Workspace
// Дерево проекта как в AS4 (скрин 7): "<имя> (default)" -> Source/Header/External Deps/Other Files
static HTREEITEM tvAdd(HWND tv, HTREEITEM parent, const char* text, LPARAM lp) {
    TVINSERTSTRUCTA is = {0};
    is.hParent = parent;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | (lp ? TVIF_PARAM : 0);
    is.item.pszText = (LPSTR)text;
    is.item.lParam = lp;
    return (HTREEITEM)SendMessageA(tv, TVM_INSERTITEMA, 0, (LPARAM)&is);
}

static void fillWorkspaceTree() {
    if (!g_wsTree) return;
    SendMessageA(g_wsTree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_wsTree);
    g_wsFiles.clear();
    if (!g_projOpen) {
        SendMessageA(g_wsTree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_wsTree, nullptr, TRUE);
        return;
    }
    HTREEITEM root = tvAdd(g_wsTree, TVI_ROOT, (folderNameOf(g_projRoot) + " (default)").c_str(), 0);
    // исходники: main.c / main.asm
    HTREEITEM nSrc = tvAdd(g_wsTree, root, "Source Files", 0);
    g_wsFiles.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_projRoot + "/*.c").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string path = g_projRoot + "/" + fd.cFileName;
            g_wsFiles.push_back(path);
            tvAdd(g_wsTree, nSrc, fd.cFileName, (LPARAM)g_wsFiles.size());  // 1-based индекс
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    h = FindFirstFileA((g_projRoot + "/*.asm").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string path = g_projRoot + "/" + fd.cFileName;
            g_wsFiles.push_back(path);
            tvAdd(g_wsTree, nSrc, fd.cFileName, (LPARAM)g_wsFiles.size());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    tvAdd(g_wsTree, root, "Header Files", 0);
    tvAdd(g_wsTree, root, "External Dependencies", 0);
    HTREEITEM nOther = tvAdd(g_wsTree, root, "Other Files", 0);
    const char* others[] = { "echo.ctos", "project.avrs" };
    for (const char* o : others)
        if (fileExists(g_projRoot + "/" + o)) {
            g_wsFiles.push_back(g_projRoot + "/" + o);
            tvAdd(g_wsTree, nOther, o, (LPARAM)g_wsFiles.size());
        }
    SendMessageA(g_wsTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_wsTree, nullptr, TRUE);
    TreeView_Expand(g_wsTree, root, TVE_EXPAND);
    TreeView_Expand(g_wsTree, nSrc, TVE_EXPAND);      // как в AS4: main.c виден сразу (скрин 7)
    TreeView_Expand(g_wsTree, nOther, TVE_EXPAND);
}

// ------------------------------------------------------------ Страницы нижней зоны
// 0 Build / 1 Message / 2 Find in Files / 3 Breakpoints (скрин 15) + вкладки СНИЗУ
static HWND outPageWnd(int page) {
    switch (page) {
        case 0: return g_buildEdit;
        case 2: return g_findEdit;
        case 3: return g_bpList;
        default: return g_msgs;
    }
}

static const char* outPageName(int page) {
    const char* names[4] = { "Build", "Message", "Find in Files", "Breakpoints and Tracepoints" };
    return names[page];
}

static void outShowPage(int page) {
    if (page < 0 || page > 3 || !g_outTabs) return;
    g_outPage = page;
    for (int i = 0; i < 4; i++)
        ShowWindow(outPageWnd(i), i == page ? SW_SHOW : SW_HIDE);
    SendMessageA(g_outTabs, TCM_SETCURSEL, page, 0);
    SetWindowTextA(g_outCap, outPageName(page));
}

// вкладка Workspace: 0 AVR GCC (дерево) / 1 Processor (список значений, как скрин 16)
static void wsShowTab(int page) {
    if (!g_wsTabs) return;
    ShowWindow(g_wsTree, page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wsList, page == 1 ? SW_SHOW : SW_HIDE);
    SendMessageA(g_wsTabs, TCM_SETCURSEL, page, 0);
    SetWindowTextA(g_wsCap, page == 0 ? "AVR GCC" : "Processor");
    if (page == 1) refreshProcWin();
}

// --------------------------------------------------- Портативный режим
// В сборке "one exe" 207 профилей чипов зашиты ресурсом (res/portable.rc):
// если рядом с exe нет каталога Procs - распаковываем из ресурса при старте.
static std::string g_portableNote;   // заметка для Message-панели после создания окна

static void ensurePortableProcs() {
    std::string dir = g_appDir + "/../Procs";
    if (dir.find("..") != std::string::npos) {
        // нормализуем .. для FindFirstFile/CreateProcess
        char full[MAX_PATH]; GetFullPathNameA(dir.c_str(), MAX_PATH, full, nullptr);
        dir = full;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*.proc").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return; }   // каталог уже с профилями

    HRSRC rs = FindResourceA(nullptr, MAKEINTRESOURCEA(1), RT_RCDATA);
    if (!rs) return;                                           // не портативная сборка - будет "профилям нет"
    HGLOBAL gl = LoadResource(nullptr, rs);
    const unsigned char* p = (const unsigned char*)LockResource(gl);
    DWORD sz = SizeofResource(nullptr, rs);
    if (!p || sz < 4) return;
    DWORD n = *(const DWORD*)p;
    const unsigned char* q = p + 4;
    CreateDirectoryA(dir.c_str(), nullptr);
    DWORD written = 0;
    for (DWORD f = 0; f < n && (DWORD)(q - p) < sz; f++) {
        WORD nameLen = *(const WORD*)q; q += 2;
        std::string name((const char*)q, nameLen); q += nameLen;
        DWORD dataLen = *(const DWORD*)q; q += 4;
        std::string path = dir + "/" + name;
        FILE* fo = fopen(path.c_str(), "wb");
        if (fo) { fwrite(q, 1, dataLen, fo); fclose(fo); written++; }
        q += dataLen;
    }
    g_portableNote = std::string("Portable: extracted ") + std::to_string(written) +
                     " device profiles -> " + dir;
}

static void browseLocation(HWND h, int editId) {    char buf[MAX_PATH] = "";
    BROWSEINFOA bi = {0};
    bi.hwndOwner = h; bi.lpszTitle = "Выберите папку проекта"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        char p[MAX_PATH]; SHGetPathFromIDListA(pidl, p);
        CoTaskMemFree(pidl);
        SetDlgItemTextA(h, editId, p);
    }
}

// создать проект по данным мастера (скрин 2/6)
static std::string g_wizType = "AVR GCC", g_wizDevice = "ATmega8A";
static bool createProject(HWND h) {
    char name[MAX_PATH], loc[MAX_PATH], initF[MAX_PATH];
    GetDlgItemTextA(h, 210, name, MAX_PATH);              // Project name
    GetDlgItemTextA(h, 213, loc, MAX_PATH);               // Location
    GetDlgItemTextA(h, 212, initF, MAX_PATH);             // Initial file
    if (!name[0] || !loc[0]) { MessageBoxA(h, "Имя проекта и папка обязательны.", "Новый проект", MB_OK | MB_ICONWARNING); return false; }
    std::string dir = std::string(loc) + "\\" + name;
    if (GetFileAttributesA(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(dir.c_str(), nullptr)) {
            MessageBoxA(h, "Не удалось создать папку проекта.", "Новый проект", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    bool isAsm = g_wizType.find("Assembler") != std::string::npos;
    std::string srcName = initF[0] ? initF : (isAsm ? "main.asm" : "main.c");
    writeTextFile(dir + "/echo.ctos", echoTemplate(g_wizDevice.c_str()));
    if (isAsm) {
        std::string asmSrc;
        asmSrc += std::string("; ") + name + " - new project (Atmel AVR Assembler)\n";
        asmSrc += "; Device: " + g_wizDevice + "\n";
        asmSrc += ".nolist\n.include \"m8def.inc\"\n.list\n";
        asmSrc += "    rjmp    init\ninit:\n";
        asmSrc += "    ser     r16\n";
        asmSrc += "    out     0x17, r16   ; DDRB = 0xFF\n";
        asmSrc += "loop:\n    rjmp    loop\n";
        writeTextFile(dir + "/" + srcName, asmSrc);
    } else {
        writeTextFile(dir + "/" + srcName, kBlinkTemplate);
    }
    writeTextFile(dir + "/project.avrs",
        std::string("type = ") + (isAsm ? "asm" : "gcc") + "\ndevice = " + g_wizDevice + "\n");
    openProjectAt(dir);
    addRecent(name, dir);
    msgAppend((std::string("Project created: ") + dir).c_str());
    statusSet(0, g_wizDevice.c_str());
    return true;
}

// сборка проекта через avr-gcc (скрин 8, Build-меню AS4); вывод -> Message
static void msgAppendStr(const std::string& s) { msgAppend(s.c_str()); }

// зеркало вывода сборки в страницу Build нижней зоны (скрин 15)
static void msgAndBuild(const char* s) {
    // Message-панель
    int n = GetWindowTextLengthA(g_msgs);
    if (n < 0) n = 0;
    std::string cur(n, 0);
    if (n > 0) SendMessageA(g_msgs, WM_GETTEXT, n + 1, (LPARAM)&cur[0]);
    if (n > 0 && !cur.empty() && cur.back() != '\n') cur += "\r\n";
    cur += s;
    SendMessageA(g_msgs, WM_SETTEXT, 0, (LPARAM)cur.c_str());
    // страница Build
    if (g_buildEdit) {
        int m = GetWindowTextLengthA(g_buildEdit);
        if (m < 0) m = 0;
        std::string cur2(m, 0);
        if (m > 0) SendMessageA(g_buildEdit, WM_GETTEXT, m + 1, (LPARAM)&cur2[0]);
        if (m > 0 && !cur2.empty() && cur2.back() != '\n') cur2 += "\r\n";
        cur2 += s;
        SendMessageA(g_buildEdit, WM_SETTEXT, 0, (LPARAM)cur2.c_str());
    }
}

static void runToolAppend(const std::string& cmdline, const std::string& cwd) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rPipe = nullptr, wPipe = nullptr;
    CreatePipe(&rPipe, &wPipe, &sa, 0);
    SetHandleInformation(wPipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wPipe; si.hStdError = wPipe;
    PROCESS_INFORMATION pi = {};
    std::string cmd = cmdline;
    BOOL ok = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wPipe);
    if (!ok) { msgAppendStr("! Не запустить: " + cmdline); CloseHandle(rPipe); return; }
    char buf[4096]; DWORD n = 0;
    std::string out;
    while (ReadFile(rPipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = 0; out += buf;
        if (out.size() > 65536) { msgAndBuild(out.c_str()); out.clear(); }
    }
    CloseHandle(rPipe);
    if (!out.empty()) msgAndBuild(out.c_str());
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    msgAndBuild((std::string(code ? "! Build FAILED (code=" : "Build OK (code=") +
               std::to_string((DWORD)code) + ")").c_str());
}

static void buildProject(bool andRun) {
    if (!g_projOpen) { msgAppend("! Нет открытого проекта. Project > New/Open Project."); return; }
    saveEditor();
    auto cfg = readCtos(g_projRoot);
    std::string tcBin = cfg.count("toolchainbin") ? cfg["toolchainbin"] :
        std::string("C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/bin");
    std::string gcc = tcBin + "/avr-gcc.exe";
    if (!fileExists(gcc)) { msgAppendStr("! avr-gcc не найден: " + gcc); return; }
    std::string src = g_srcPath;
    if (!fileExists(src)) { msgAppendStr("! Нет исходника: " + src); return; }
    std::string opt = cfg.count("optimization") ? cfg["optimization"] : std::string("O0");
    std::string elf = "\"" + g_projRoot + "/main.elf\"";
    msgAppend((std::string("-- Build (device ") + g_deviceName + ", mcu " + g_mcuName + ", -" + opt + ")").c_str());
    // avr-gcc 4.5.1: не все современные суффиксы знает - пробуем полный mcu,
    // при неудаче укорачиваем имя по букве (atmega8a -> atmega8) и ретраим
    std::string mcu = g_mcuName;
    std::string gccBase = "\"" + gcc + "\"";
    for (int attempt = 0; attempt < 4; attempt++) {
        std::string cmdline = gccBase + " -mmcu=" + mcu + " -g -" + opt +
                              " -DF_CPU=8000000UL -o " + elf +
                              " \"" + src + "\"";
        if (attempt) msgAppend((std::string("-- retry: -mmcu=") + mcu).c_str());
        runToolAppend(cmdline, g_projRoot);
        if (fileExists(g_projRoot + "/main.elf")) break;
        if (mcu.size() > 2) mcu.pop_back(); else break;
    }
    if (andRun && fileExists(g_projRoot + "/main.elf")) {
        msgAppend("-- Build and Run: запуск отладки");
        startDebugging();
        if (g_coreInit && emuapi_loaded() && !g_runBusy) {
            g_runBusy = true;
            CreateThread(nullptr, 0, runThreadProc, nullptr, 0, nullptr);
        }
    }
}

// ------------------------------------------------------------- Welcome (скрин 1)
// Popup-окно вместо DLGTEMPLATE: надёжно, позиция контролов в пикселях.
static char g_recentName[200];   // выбранный recent

static HFONT g_uiFont = nullptr, g_uiFontBig = nullptr;

static HFONT uiFont() {
    if (!g_uiFont) g_uiFont = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, RUSSIAN_CHARSET,
        0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    return g_uiFont;
}
static HFONT uiFontBig() {
    if (!g_uiFontBig) g_uiFontBig = CreateFontA(-26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, RUSSIAN_CHARSET,
        0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    return g_uiFontBig;
}

static HWND mkCtrl(HWND parent, const char* cls, const char* title, DWORD style,
                   int x, int y, int cx, int cy, int id, HFONT font) {
    HWND c = CreateWindowExA(0, cls, title, WS_CHILD | WS_VISIBLE | style,
                             x, y, cx, cy, parent, (HMENU)(intptr_t)id, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)(font ? font : uiFont()), TRUE);
    return c;
}

// центрирование относительно владельца
static void centerPopup(HWND h, HWND owner, int w, int hgt, int* outX, int* outY) {
    RECT orr = {0, 0, 1200, 800};
    if (owner) GetWindowRect(owner, &orr);
    *outX = orr.left + ((orr.right - orr.left) - w) / 2;
    *outY = orr.top + ((orr.bottom - orr.top) - hgt) / 2;
}

struct WelState { HWND h; HWND owner; int result; };
static WelState g_wel = {nullptr, nullptr, 0};

static void welClose(HWND h, int result) {
    g_wel.result = result;
    DestroyWindow(h);
}

static LRESULT CALLBACK WelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            mkCtrl(h, "Static", "Welcome to AVR Studio 4", SS_LEFT, 20, 14, 420, 36, 500, uiFontBig());
            mkCtrl(h, "Button", "&New Project", BS_DEFPUSHBUTTON, 20, 64, 150, 38, 501, nullptr);
            mkCtrl(h, "Button", "&Open Project...", BS_PUSHBUTTON, 20, 108, 150, 38, 502, nullptr);
            mkCtrl(h, "Button", "Recent projects", BS_GROUPBOX, 184, 56, 262, 96, 0, nullptr);
            HWND lb = mkCtrl(h, "ListBox", "", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | LBS_HASSTRINGS,
                             196, 72, 238, 72, 503, nullptr);
            for (auto& r : g_recent) SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)r.name.c_str());
            mkCtrl(h, "Button", "Show dialog at startup", BS_AUTOCHECKBOX, 20, 300, 190, 20, 504, nullptr);
            mkCtrl(h, "Button", "Cancel", BS_PUSHBUTTON, 370, 298, 76, 24, IDCANCEL, nullptr);
            SendMessageA(GetDlgItem(h, 504), BM_SETCHECK, BST_CHECKED, 0);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case 501: welClose(h, 1); return 0;          // New Project
                case 502: welClose(h, 2); return 0;          // Open Project
                case IDCANCEL: welClose(h, 0); return 0;
                case 503:
                    if (HIWORD(w) == LBN_DBLCLK) {
                        int sel = SendDlgItemMessageA(h, 503, LB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < (int)g_recent.size()) {
                            strcpy(g_recentName, g_recent[sel].path.c_str());
                            welClose(h, 3); return 0;
                        }
                    }
                    break;
            }
            break;
        case WM_CLOSE:
            welClose(h, 0); return 0;
        case WM_DESTROY:
            if (g_wel.h == h) g_wel.h = nullptr;
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// модальный цикл для popup-окна (владелец заблокирован)
static int runModalPopup(HWND popup, HWND owner, HWND* alive, int* result) {
    EnableWindow(owner, FALSE);
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    MSG msg;
    while (*alive && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return result ? *result : 0;
}

static const char kWelClass[] = "AVRCLI_Welcome";

static int showWelcome(HWND parent) {
    if (!parent) parent = g_main;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WelProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kWelClass;
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    int W = 480, H = 380, x, y;
    centerPopup(nullptr, parent, W, H, &x, &y);
    g_wel.h = CreateWindowExA(WS_EX_DLGMODALFRAME, kWelClass, "Welcome to AVR Studio 4",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr,
        GetModuleHandleA(nullptr), nullptr);
    g_wel.owner = parent;
    g_wel.result = 0;
    return runModalPopup(g_wel.h, parent, &g_wel.h, &g_wel.result);
}

// ------------------------------------------------------------- Мастер проекта (скрины 2/5/6)
static const int kWizPage1[] = { 211, 220, 210, 225, 226, 212, 228, 213, 229, 231 };
static const int kWizPage2[] = { 340, 344, 341, 342, 343 };

static void wizShowPage(HWND h, int page) {
    for (int id : kWizPage1) ShowWindow(GetDlgItem(h, id), page == 1 ? SW_SHOW : SW_HIDE);
    for (int id : kWizPage2) ShowWindow(GetDlgItem(h, id), page == 2 ? SW_SHOW : SW_HIDE);
    SetFocus(GetDlgItem(h, page == 1 ? 211 : 341));
}

struct WizState { HWND h; HWND owner; int finished; };
static WizState g_wiz = {nullptr, nullptr, false};

static void wizClose(HWND h, bool finished) {
    g_wiz.finished = finished;
    DestroyWindow(h);
}

static LRESULT CALLBACK WizProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            // страница 1: тип/имя/исходник/папка (скрин 2)
            mkCtrl(h, "Static", "1. What do you want to do?", SS_LEFT, 16, 14, 300, 20, 0, nullptr);
            HWND lb = mkCtrl(h, "ListBox", "", LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
                             16, 38, 280, 52, 211, nullptr);
            SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"Atmel AVR Assembler");
            SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"AVR GCC");
            SendMessageA(lb, LB_SETCURSEL, 1, 0);
            g_wiz.h = h; g_wiz.finished = false; g_wiz.owner = (HWND)((CREATESTRUCTA*)l)->lpCreateParams;
            mkCtrl(h, "Static", "Project name:", SS_LEFT, 16, 108, 90, 18, 220, nullptr);
            mkCtrl(h, "Edit", "", ES_LEFT | WS_BORDER | WS_TABSTOP, 110, 104, 250, 24, 210, nullptr);
            mkCtrl(h, "Button", "Create initial file", BS_AUTOCHECKBOX, 16, 140, 160, 20, 225, nullptr);
            mkCtrl(h, "Static", "Initial file:", SS_LEFT, 16, 170, 80, 18, 226, nullptr);
            mkCtrl(h, "Edit", "", ES_LEFT | WS_BORDER | WS_TABSTOP, 110, 166, 250, 24, 212, nullptr);
            mkCtrl(h, "Static", "Location:", SS_LEFT, 16, 200, 80, 18, 228, nullptr);
            mkCtrl(h, "Edit", "", ES_LEFT | WS_BORDER | WS_TABSTOP, 110, 196, 180, 24, 213, nullptr);
            mkCtrl(h, "Button", "Browse...", BS_PUSHBUTTON, 296, 196, 64, 24, 229, nullptr);
            mkCtrl(h, "Button", "Create folder", BS_AUTOCHECKBOX, 16, 230, 120, 20, 231, nullptr);
            // страница 2: платформа + чип (скрины 5/6; чипы из каталога Procs)
            mkCtrl(h, "Static", "2. Select debug platform and device", SS_LEFT, 16, 14, 320, 20, 340, nullptr);
            mkCtrl(h, "Static", "Debug platform:", SS_LEFT, 16, 40, 110, 18, 344, nullptr);
            lb = mkCtrl(h, "ListBox", "", LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
                        16, 60, 210, 180, 341, nullptr);
            const char* plats[] = { "AVR Dragon", "AVR ONE!", "AVR Simulator", "AVR Simulator 2",
                                    "ICE200", "ICE40", "ICE50", "JTAG ICE", "JTAGICE mkII" };
            for (auto p : plats) SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)p);
            SendMessageA(lb, LB_SETCURSEL, 3, 0);               // AVR Simulator 2
            mkCtrl(h, "Static", "Device:", SS_LEFT, 240, 40, 110, 18, 342, nullptr);
            lb = mkCtrl(h, "ListBox", "", LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
                        240, 60, 240, 268, 343, nullptr);
            auto devs = listDevices(g_appDir + "/../Procs");
            int devSel = -1, prefIdx = -1;
            for (size_t i = 0; i < devs.size(); i++) {
                SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)devs[i].c_str());
                if (devs[i] == "ATmega8") prefIdx = (int)i;
                if (devs[i] == g_wizDevice) devSel = (int)i;
            }
            SendMessageA(lb, LB_SETCURSEL, devSel >= 0 ? devSel : (prefIdx >= 0 ? prefIdx : 0), 0);
            // полоса кнопок внизу
            mkCtrl(h, "Button", "<< Back", BS_PUSHBUTTON, 16, 386, 70, 26, 320, nullptr);
            mkCtrl(h, "Button", "Next >", BS_DEFPUSHBUTTON, 400, 386, 70, 26, 321, nullptr);
            mkCtrl(h, "Button", "Fin&ish", BS_PUSHBUTTON, 474, 386, 70, 26, IDOK, nullptr);
            mkCtrl(h, "Button", "Cancel", BS_PUSHBUTTON, 548, 386, 70, 26, IDCANCEL, nullptr);
            SetDlgItemTextA(h, 210, "project1");
            SetDlgItemTextA(h, 212, "main.c");
            SendMessageA(GetDlgItem(h, 225), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageA(GetDlgItem(h, 231), BM_SETCHECK, BST_CHECKED, 0);
            std::string loc = g_appDir + "/..";
            SetDlgItemTextA(h, 213, loc.c_str());               // стартовая точка местоположения
            wizShowPage(h, 1);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case 211:   // выбрали тип
                    if (HIWORD(w) == LBN_SELCHANGE) {
                        char t[32]; GetDlgItemTextA(h, 211, t, sizeof(t));
                        g_wizType = t;
                        SetDlgItemTextA(h, 212, std::string(t).find("Assembler") != std::string::npos ? "main.asm" : "main.c");
                    }
                    break;
                case 343:
                    if (HIWORD(w) == LBN_SELCHANGE) {
                        char d[64]; GetDlgItemTextA(h, 343, d, sizeof(d));
                        g_wizDevice = d;
                    }
                    break;
                case 229:   // Browse...
                    browseLocation(h, 213);
                    break;
                case 320:   // << Back
                    wizShowPage(h, 1);
                    break;
                case 321: { // Next >
                    char nm[MAX_PATH], loc[MAX_PATH];
                    GetDlgItemTextA(h, 210, nm, MAX_PATH);
                    GetDlgItemTextA(h, 213, loc, MAX_PATH);
                    if (!nm[0] || !loc[0]) { MessageBoxA(h, "Укажите имя проекта и папку.", "Wizard", MB_OK | MB_ICONWARNING); break; }
                    wizShowPage(h, 2);
                    break;
                }
                case IDOK: {   // Finish
                    if (!createProject(h)) return 0;
                    wizClose(h, true);
                    return 0;
                }
                case IDCANCEL:
                    wizClose(h, false);
                    return 0;
            }
            break;
        case WM_CLOSE:
            wizClose(h, false); return 0;
        case WM_DESTROY:
            if (g_wiz.h == h) g_wiz.h = nullptr;
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static const char kWizClass[] = "AVRCLI_Wizard";

static bool showWizard(HWND parent) {
    if (!parent) parent = g_main;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WizProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kWizClass;
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    int W = 660, H = 470, x, y;
    centerPopup(nullptr, parent, W, H, &x, &y);
    g_wiz.h = CreateWindowExA(WS_EX_DLGMODALFRAME, kWizClass, "AVR Studio Wizard. New Project",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr,
        GetModuleHandleA(nullptr), nullptr);
    // WizProc создаёт контролы, потом модальный цикл
    g_wiz.finished = false;
    runModalPopup(g_wiz.h, parent, &g_wiz.h, &g_wiz.finished);
    return g_wiz.finished;
}

// Project > Open Project (GetOpenFileNameA для .avrs / echo.ctos)
static char g_openBuf[MAX_PATH * 3];

static void openProjectDialog(HWND h) {
    OPENFILENAMEA of = {0};
    g_openBuf[0] = 0;
    of.lStructSize = sizeof(of);
    of.hwndOwner = h;
    of.lpstrFilter = "Project Files (*.avrs, echo.ctos)\0*.avrs;echo.ctos\0All Files (*.*)\0*.*\0";
    of.lpstrFile = g_openBuf;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = "Open Project";
    of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&of)) return;
    std::string p = g_openBuf;
    std::string dir = p;
    size_t sl = dir.find_last_of("\\/");
    dir = sl != std::string::npos ? dir.substr(0, sl) : dir;
    openProjectAt(dir);
    addRecent(folderNameOf(dir), dir);
}

// --------------------------------------------------------------- Main proc
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE: {
            // тулбары не стакаются автоматически — кладу вручную
            SendMessageA(g_tbStd, TB_AUTOSIZE, 0, 0);
            SendMessageA(g_tbDbg, TB_AUTOSIZE, 0, 0);
            DWORD bsz1 = SendMessageA(g_tbStd, TB_GETBUTTONSIZE, 0, 0), bsz2 = SendMessageA(g_tbDbg, TB_GETBUTTONSIZE, 0, 0);
            int hStd = HIWORD(bsz1) > 16 ? HIWORD(bsz1) : 26;  // TB_GETBUTTONSIZE врёт при пустом шрифте — минимум 26
            int hDbg = HIWORD(bsz2) > 16 ? HIWORD(bsz2) : 26;
            int cxw = LOWORD(l);
            MoveWindow(g_tbStd, 0, 0, cxw, hStd, TRUE);
            MoveWindow(g_tbDbg, 0, hStd, cxw, hDbg, TRUE);
            int yTop = hStd + hDbg;
            // статусбар внизу
            SendMessageA(g_status, WM_SIZE, 0, 0);
            int statusH = 0;
            RECT rs; GetWindowRect(g_status, &rs); statusH = rs.bottom - rs.top;
            int cx = LOWORD(l), cy = HIWORD(l);
            int leftW   = 230;   // Workspace-док (скрин 7)
            int rightW  = 240;   // I/O View док
            int bottomH = 170;   // нижняя зона вывода (скрин 15)
            int workH = cy - yTop - statusH;
            int contentH = workH - bottomH;      // высота зоны редактор/доки
            int capH = 20, tabsH = 26;
            // нижняя зона вывода на всю ширину окна (скрин 15: вкладки у нижней кромки)
            int bY = yTop + contentH;
            MoveWindow(g_outCap,    0, bY, cx, capH, TRUE);
            int cH = bottomH - capH - tabsH;
            MoveWindow(g_msgs,      0, bY + capH, cx, cH, TRUE);
            MoveWindow(g_buildEdit, 0, bY + capH, cx, cH, TRUE);
            MoveWindow(g_findEdit,  0, bY + capH, cx, cH, TRUE);
            MoveWindow(g_bpList,    0, bY + capH, cx, cH, TRUE);
            MoveWindow(g_outTabs,   0, cy - statusH - tabsH, cx, tabsH, TRUE);
            // Workspace-док слева (дерево проекта / процессорный список)
            MoveWindow(g_wsCap,  0, yTop, leftW, capH, TRUE);
            int wsH = contentH - capH - 24;
            MoveWindow(g_wsTree, 0, yTop + capH, leftW, wsH, TRUE);
            MoveWindow(g_wsList, 0, yTop + capH, leftW, wsH, TRUE);
            MoveWindow(g_wsTabs, 0, yTop + contentH - 24, leftW, 24, TRUE);
            // центральный редактор
            MoveWindow(g_editMain, leftW, yTop, cx - leftW - rightW, contentH, TRUE);
            // правый док: сверху модули, снизу регистры
            int ioHalf = contentH / 2;
            MoveWindow(g_ioTree, cx - rightW, yTop, rightW, ioHalf, TRUE);
            MoveWindow(g_ioList, cx - rightW, yTop + ioHalf, rightW, contentH - ioHalf, TRUE);
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)l;
            if (nm->code == TCN_SELCHANGE && nm->hwndFrom == g_outTabs)
                outShowPage((int)SendMessageA(g_outTabs, TCM_GETCURSEL, 0, 0));
            else if (nm->code == TCN_SELCHANGE && nm->hwndFrom == g_wsTabs)
                wsShowTab((int)SendMessageA(g_wsTabs, TCM_GETCURSEL, 0, 0));
            else if (nm->code == NM_DBLCLK && nm->hwndFrom == g_wsTree) {
                HTREEITEM sel = TreeView_GetSelection(g_wsTree);
                if (sel) {
                    TVITEMA ti = {0};
                    ti.mask = TVIF_PARAM; ti.hItem = sel;
                    if (TreeView_GetItem(g_wsTree, &ti) && ti.lParam > 0) {
                        int idx = (int)ti.lParam - 1;
                        if (idx >= 0 && idx < (int)g_wsFiles.size())
                            loadEditorFromFile(g_wsFiles[idx]);
                    }
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            if (l != 0) return 0;   // уведомление контрола (EN_* и пр.), не меню — иначе рекурсия в обработчик
            int id = LOWORD(w);
            if (id == IDM_FILE_EXIT) { PostQuitMessage(0); return 0; }
            if (id == IDM_HELP_ABOUT) {
                MessageBoxA(h, "AVR CLI Studio - бесплатный аналог AVR Studio 4.\n"
                               "GUI + нативный эмулятор C++ (ядро встроено в процесс).",
                            "About AVR Studio...", MB_OK);
                return 0;
            }
            switch (id) {
                case IDM_DBG_START:      // Start Debugging
                    startDebugging();
                    refreshIoListView();
            refreshProcWin();
                    return 0;
                case IDM_DBG_RUN:        // Run (F5) - worker-поток, UI живёт
                    if (!g_coreInit) { msgAppend("! Прогон: сначала Debug > Start Debugging (Ctrl+Shift+Alt+F5)"); return 0; }
                    if (g_runBusy) { msgAppend("-- Run уже идёт (Break = Ctrl+F5)"); return 0; }
                    g_runBusy = true;
                    msgAppend("-- Run (F5)...");
                    CloseHandle(CreateThread(nullptr, 0, runThreadProc, nullptr, 0, nullptr));
                    return 0;
                case IDM_DBG_BREAK:      // Break (Ctrl+F5)
                    emuapi_stop();
                    msgAppend("-- Break");
                    return 0;
                case IDM_DBG_RESET:      // Reset (Shift+F5)
                    if (g_coreInit) { emuapi_reset(); refreshIoListView();
            refreshProcWin(); }
                    return 0;
                case IDM_DBG_STEPIN:     // Step Into (F11) = машинный шаг
                    if (!g_coreInit || !emuapi_loaded()) { msgAppend("! Сначала Debug > Start Debugging"); return 0; }
                    if (emuapi_stepInstrs(1)) { refreshIoListView();
            refreshProcWin(); }
                    return 0;
                case IDM_DBG_STEPOVER:   // Step Over (F10) = C-строка over (DWARF)
                    if (!g_coreInit || !emuapi_loaded()) { msgAppend("! Сначала Debug > Start Debugging"); return 0; }
                    emuapi_cmd("next");
                    refreshIoListView();
            refreshProcWin();
                    return 0;
                case IDM_DBG_STEPOUT:    // Step Out (Shift+F11)
                    if (!g_coreInit || !emuapi_loaded()) { msgAppend("! Сначала Debug > Start Debugging"); return 0; }
                    emuapi_cmd("finish");
                    refreshIoListView();
            refreshProcWin();
                    return 0;
                case IDM_DBG_STOP:       // Stop Debugging
                    emuapi_stop();
                    msgAppend("-- Debugging stopped");
                    return 0;
                // ---- Project ----
                case IDM_PROJ_WIZARD:
                case IDM_PROJ_NEW:
                    showWizard(h);
                    return 0;
                case IDM_PROJ_OPEN:
                    openProjectDialog(h);
                    return 0;
                case IDM_PROJ_SAVE:      // пересохранение echo.ctos пока не нужно - project.avrs уже на диске
                    msgAppend("Project state is saved on disk (echo.ctos)");
                    return 0;
                // ---- Build ----
                case IDM_BUILD_COMPILE:
                case IDM_BUILD_BUILD:
                    buildProject(false);
                    return 0;
                case IDM_BUILD_RUN:
                    buildProject(true);
                    return 0;
                case IDM_BUILD_CLEAN:
                    if (g_projOpen) {
                        std::string elf = g_projRoot + "/main.elf";
                        if (DeleteFileA(elf.c_str())) msgAppend("-- Cleaned: main.elf удалён");
                        else msgAppend("-- Cleaned: main.elf не найден");
                    }
                    return 0;
                // ---- Edit ----
                case IDM_EDIT_UNDO:   SendMessageA(g_editMain, WM_UNDO, 0, 0); return 0;
                case IDM_EDIT_REDO:   SendMessageA(g_editMain, 0x030B, 0, 0); return 0;  // EM_REDO
                case IDM_EDIT_CUT:    SendMessageA(g_editMain, WM_CUT, 0, 0); return 0;
                case IDM_EDIT_COPY:   SendMessageA(g_editMain, WM_COPY, 0, 0); return 0;
                case IDM_EDIT_PASTE:  SendMessageA(g_editMain, WM_PASTE, 0, 0); return 0;
                case IDM_EDIT_DELETE: SendMessageA(g_editMain, WM_CLEAR, 0, 0); return 0;
                case IDM_EDIT_SELALL: SendMessageA(g_editMain, EM_SETSEL, 0, -1); return 0;
                case IDM_EDIT_BMTGGL:
                case IDM_EDIT_BMPREV:
                case IDM_EDIT_BMNEXT:
                case IDM_EDIT_BMRMALL:
                    msgAppend("-- Bookmarks: заглушка, реализуются в следующей вехе");
                    return 0;
                // ---- Window ----
                case IDM_WIN_CASCADE:
                case IDM_WIN_TILEH:
                case IDM_WIN_TILEV:
                case IDM_WIN_ARRANGE:
                    msgAppend("-- Window layout: единое окно редактора (MDI-дочки появятся в следующей вехе)");
                    return 0;
                case IDM_WIN_LIST:
                    SetFocus(g_editMain);
                    return 0;
                // ---- View ----
                case IDM_VIEW_PROC:
                    showProcWin(h);
                    return 0;
                case IDM_VIEW_BUILDOUT: outShowPage(0); return 0;
                case IDM_VIEW_MSGOUT:   outShowPage(1); return 0;
                case IDM_VIEW_FINDOUT:  outShowPage(2); return 0;
                case IDM_VIEW_BREAKS:   outShowPage(3); return 0;
                // ---- File ----
                case IDM_FILE_SAVE:
                case IDM_FILE_SAVEALL:
                    saveEditor();
                    return 0;                case IDM_FILE_PRINT:
                case IDM_FILE_PRINTSETUP:
                case IDM_FILE_CLOSE:
                case IDM_FILE_SAVEAS:
                case IDM_FILE_NEW:
                case IDM_FILE_OPEN:
                    msgAppend((std::string("[menu] id=") + std::to_string(id)).c_str());
                    return 0;
            }
            msgAppend((std::string("[menu] id=") + std::to_string(id)).c_str());
            return 0;
        }
        case WM_APP + 1:  // прогон завершён (из worker-потока)
            refreshIoListView();
            refreshProcWin();
            msgAppend("-- Run ended");
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetUnhandledExceptionFilter(crashFilter);
    g_appDir = exeDirNow();
    loadRecent();
    ensurePortableProcs();
    registerCapClass();
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES
        | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_fontUi = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, RUSSIAN_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);  // светло-серый, как у AS4 (скрин 1)
    wc.lpszClassName = "AVRCLIStudio_Main";
    RegisterClassA(&wc);

    // Меню 1:1 AVR Studio 4 (скрины 8..10)
    HMENU mb = CreateMenu();
    {   HMENU mFile = CreateMenu(), mProj = CreateMenu(), mBuild = CreateMenu();
        HMENU mView = CreateMenu(), mTools = CreateMenu(), mDbg = CreateMenu(), mHelp = CreateMenu();
        HMENU mEdit = CreateMenu(), mWin = CreateMenu();
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mFile,  "&File");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mProj,  "&Project");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mBuild, "&Build");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mEdit,  "&Edit");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mView,  "&View");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mTools, "&Tools");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mDbg,   "&Debug");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mWin,   "&Window");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mHelp,  "&Help");
        // Edit (скрин 9/12: Undo/Redo/Cut/Copy/Paste/Delete/Select All/Bookmarks)
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_UNDO,   "&Undo\tCtrl+Z");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_REDO,   "&Redo\tCtrl+Y");
        AppendMenuA(mEdit, MF_SEPARATOR, 0, 0);
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_CUT,    "Cu&t\tCtrl+X");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_COPY,   "&Copy\tCtrl+C");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_PASTE,  "&Paste\tCtrl+V");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_DELETE, "De&lete\tDel");
        AppendMenuA(mEdit, MF_SEPARATOR, 0, 0);
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_SELALL, "Select A&ll\tCtrl+A");
        AppendMenuA(mEdit, MF_SEPARATOR, 0, 0);
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_BMTGGL, "Toggle Bookmark\tCtrl+F2");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_BMPREV, "Previous Bookmark\tShift+F2");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_BMNEXT, "Next Bookmark\tF2");
        AppendMenuA(mEdit, MF_STRING, IDM_EDIT_BMRMALL,"Remove All Bookmarks");
        // Window (каскад/тайлинг и список открытых окон)
        AppendMenuA(mWin, MF_STRING, IDM_WIN_CASCADE, "&Cascade");
        AppendMenuA(mWin, MF_STRING, IDM_WIN_TILEH,  "Tile &Horizontal");
        AppendMenuA(mWin, MF_STRING, IDM_WIN_TILEV,  "Tile &Vertical");
        AppendMenuA(mWin, MF_STRING, IDM_WIN_ARRANGE,"&Arrange Icons");
        AppendMenuA(mWin, MF_SEPARATOR, 0, 0);
        AppendMenuA(mWin, MF_STRING | MF_CHECKED, IDM_WIN_LIST, "&1 AVR Studio (current)");
        // File (скрин 9: New File/Open File/Close/Save/Save As.../Save All/Print/Print Setup/Exit)
        AppendMenuA(mFile, MF_STRING, IDM_FILE_NEW,        "&New File\tCtrl+N");
        AppendMenuA(mFile, MF_STRING, IDM_FILE_OPEN,       "&Open File...\tCtrl+O");
        AppendMenuA(mFile, MF_STRING, IDM_FILE_CLOSE,      "&Close");
        AppendMenuA(mFile, MF_SEPARATOR, 0, 0);
        AppendMenuA(mFile, MF_STRING, IDM_FILE_SAVE,       "&Save\tCtrl+S");
        AppendMenuA(mFile, MF_STRING, IDM_FILE_SAVEAS,     "Save &As...");
        AppendMenuA(mFile, MF_STRING, IDM_FILE_SAVEALL,    "Save A&ll");
        AppendMenuA(mFile, MF_SEPARATOR, 0, 0);
        AppendMenuA(mFile, MF_STRING, IDM_FILE_PRINT,      "&Print...\tCtrl+P");
        AppendMenuA(mFile, MF_STRING, IDM_FILE_PRINTSETUP, "Print Setu&p...");
        AppendMenuA(mFile, MF_SEPARATOR, 0, 0);
        AppendMenuA(mFile, MF_STRING, IDM_FILE_EXIT,       "E&xit");
        // Project (скрин 9: Project Wizard/New Project/Open Project/Save/Close/Recent Projects)
        AppendMenuA(mProj, MF_STRING, IDM_PROJ_WIZARD, "Project &Wizard");
        AppendMenuA(mProj, MF_STRING, IDM_PROJ_NEW,    "&New Project");
        AppendMenuA(mProj, MF_STRING, IDM_PROJ_OPEN,   "&Open Project");
        AppendMenuA(mProj, MF_STRING, IDM_PROJ_SAVE,   "S&ave Project");
        AppendMenuA(mProj, MF_STRING, IDM_PROJ_CLOSE,  "&Close Project");
        AppendMenuA(mProj, MF_SEPARATOR, 0, 0);
        AppendMenuA(mProj, MF_STRING, IDM_PROJ_RECENT, "&Recent Projects");
        // Build (скрин 8: AVR GCC submenu)
        AppendMenuA(mBuild, MF_STRING, IDM_BUILD_COMPILE, "&Compile Active File");
        AppendMenuA(mBuild, MF_STRING, IDM_BUILD_BUILD,   "B&uild Active Configuration");
        AppendMenuA(mBuild, MF_STRING, IDM_BUILD_RUN,     "Build An&d Run");
        AppendMenuA(mBuild, MF_STRING, IDM_BUILD_CLEAN,   "C&lean Current Configuration");
        AppendMenuA(mBuild, MF_STRING, IDM_BUILD_OPTIONS, "&Edit Current Configuration Options");
        // View (скрин 8: Toolbars/Status Bar/Disassembler/Watch/Memory/Memory 2/3/Register/I/O/Processor/Output'ы)
        AppendMenuA(mView, MF_STRING, IDM_VIEW_DISASM,  "&Disassembler");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_WATCH,   "&Watch\tAlt-1");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_MEM,     "&Memory\tAlt-4");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_MEM2,    "&Memory 2");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_MEM3,    "&Memory 3");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_REG,     "&Register\tAlt-0");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_IO,      "&I/O\tAlt-5");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_PROC,    "&Processor");
        AppendMenuA(mView, MF_SEPARATOR, 0, 0);
        AppendMenuA(mView, MF_STRING, IDM_VIEW_BUILDOUT, "&Build Output");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_MSGOUT,   "&Message Output");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_FINDOUT,  "&Find Output");
        AppendMenuA(mView, MF_STRING, IDM_VIEW_BREAKS,   "&Breakpoints and Tracepoints");
        // Tools (скрин 8)
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_AVRPROG,  "AVRPr&og...");
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_CUSTOM,   "Customise...");
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_OPTIONS,  "Options...");
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_KEYS,     "Show Key Assignments");
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_PLUGIN,   "Plug-in M&anager...");
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_PROG_CONNECT, "Program AVR: Connect...");
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_PROGP_AUTO,   "Program AVR: Start Auto");
        AppendMenuA(mTools, MF_SEPARATOR, 0, 0);
        AppendMenuA(mTools, MF_STRING, IDM_TOOLS_FLIP, "FLIP3 Information");
        // Debug (скрин 10: Start/Stop Debugging, Run, Break, Reset, Step.., Run To Cursor, Next/Remove BP, Trace)
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_START,    "&Start Debugging\tCtrl+Shift+Alt+F5");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_STOP,     "Sto&p Debugging\tCtrl+Shift+F5");
        AppendMenuA(mDbg, MF_SEPARATOR, 0, 0);
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_RUN,      "&Run\tF5");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_BREAK,    "&Break\tCtrl+F5");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_RESET,    "Reset\tShift+F5");
        AppendMenuA(mDbg, MF_SEPARATOR, 0, 0);
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_STEPIN,   "Step &Into\tF11");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_STEPOVER, "Step &Over\tF10");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_STEPOUT,  "Step Ou&t\tShift+F11");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_AUTOSTEP, "&Auto Step\tAlt+F5");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_RUNTOCUR, "Run To &Cursor\tCtrl+F10");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_NEXTBP,   "&Next Breakpoint\tCtrl+F9");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_REMOVEBP, "&Remove All Breakpoints");
        AppendMenuA(mDbg, MF_SEPARATOR, 0, 0);
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_TRACE,      "&Trace");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_STACKMON,   "&Stack Monitor");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_SHOWNEXT,   "Show &Next Statement\tAlt+Num *");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_QUICKWATCH, "&Quickwatch\tShift+F9");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_PLATFORM,   "Select &Platform and Device...");
        AppendMenuA(mDbg, MF_STRING, IDM_DBG_MEMUPDOWN,  "&Up/DownLoad Memory");
        // Help (скрин 10)
        AppendMenuA(mHelp, MF_STRING, IDM_HELP_TOOLSUG, "AVR Tools User Guide");
        AppendMenuA(mHelp, MF_STRING, IDM_HELP_USERSUG, "AVR Studio User Guide");
        AppendMenuA(mHelp, MF_STRING, IDM_HELP_UPGRADE, "Check for Program Upgrade");
        AppendMenuA(mHelp, MF_STRING, IDM_HELP_RELNOTES, "Release Notes and Known Issues");
        AppendMenuA(mHelp, MF_SEPARATOR, 0, 0);
        AppendMenuA(mHelp, MF_STRING, IDM_HELP_ABOUT, "About AVR Studio...");
    }

    g_main = CreateWindowExA(0, "AVRCLIStudio_Main", "AVR Studio", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, 0, mb, hInst, 0);

    // --- Тулбары (скрин 1/7: Standard сверху, Debug под ним) — текстовые кнопки, иконки на M2
    g_tbStd = CreateWindowExA(0, TOOLBARCLASSNAMEA, 0,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_NORESIZE | CCS_NODIVIDER,
        0, 0, 1200, 26, g_main, (HMENU)210, hInst, 0);
    SendMessageA(g_tbStd, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    tbAddStr(g_tbStd, IDM_FILE_NEW,   "New", 0);
    tbAddStr(g_tbStd, IDM_FILE_OPEN,  "Open", 0);
    tbAddStr(g_tbStd, IDM_FILE_SAVE,  "Save", 0);
    tbAddStr(g_tbStd, 0, "", 1);
    tbAddStr(g_tbStd, IDM_BUILD_BUILD, "Build", 0);
    tbAddStr(g_tbStd, IDM_BUILD_COMPILE, "Build and Run", 0);
    SendMessageA(g_tbStd, TB_AUTOSIZE, 0, 0);

    g_tbDbg = CreateWindowExA(0, TOOLBARCLASSNAMEA, 0,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_NORESIZE | CCS_NODIVIDER,
        0, 0, 1200, 26, g_main, (HMENU)211, hInst, 0);
    SendMessageA(g_tbDbg, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    tbAddStr(g_tbDbg, IDM_DBG_START,   "Start", 0);
    tbAddStr(g_tbDbg, IDM_DBG_RUN,     "Run", 0);
    tbAddStr(g_tbDbg, IDM_DBG_BREAK,   "Break", 0);
    tbAddStr(g_tbDbg, 0, "", 1);
    tbAddStr(g_tbDbg, IDM_DBG_RESET,   "Reset", 0);
    tbAddStr(g_tbDbg, 0, "", 1);
    tbAddStr(g_tbDbg, IDM_DBG_STEPIN,   "Step Into", 0);
    tbAddStr(g_tbDbg, IDM_DBG_STEPOVER, "Step Over", 0);
    tbAddStr(g_tbDbg, IDM_DBG_STEPOUT,  "Step Out", 0);
    SendMessageA(g_tbDbg, TB_AUTOSIZE, 0, 0);

    // --- Центральный редактор (скрин 17: MDI-child с гуттером строк; MDI на M3, пока EDIT)
    g_editMain = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        "#include <avr/io.h>\r\n\r\nint main(void)\r\n{\r\n    DDRB = 0xFF;\r\n    PORTB = 0x01;\r\n    for(;;);\r\n}\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0, 0, 700, 400, g_main, (HMENU)205, hInst, 0);
    { HFONT fmono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, RUSSIAN_CHARSET,
            0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
      SendMessageA(g_editMain, WM_SETFONT, (WPARAM)fmono, TRUE); }

    // --- I/O View (правый док, скрин 20): сверху TreeView-модули, снизу ListView Name|Address|Value|Bits
    g_ioTree = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TREEVIEWA, 0,
        WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
        0, 0, 240, 400, g_main, (HMENU)201, hInst, 0);
    g_ioList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, 0,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 400, 240, 200, g_main, (HMENU)202, hInst, 0);
    { LVCOLUMNA lv = {0}; lv.mask = LVCF_TEXT | LVCF_WIDTH; lv.pszText = (LPSTR)"Name"; lv.cx = 120;
      ListView_InsertColumn(g_ioList, 0, &lv);
      lv.pszText = (LPSTR)"Address"; lv.cx = 60; ListView_InsertColumn(g_ioList, 1, &lv);
      lv.pszText = (LPSTR)"Value";  lv.cx = 50; ListView_InsertColumn(g_ioList, 2, &lv);
      lv.pszText = (LPSTR)"Bits";   lv.cx = 60; ListView_InsertColumn(g_ioList, 3, &lv); }
    fillIoTree(g_ioTree);

    // --- Нижняя зона вывода (скрин 15): caption сверху + страницы + вкладки СНИЗУ
    g_outCap = CreateWindowExA(0, "AVRCLI_Cap", "Message",
        WS_CHILD | WS_VISIBLE, 0, 0, 700, 20, g_main, (HMENU)231, hInst, 0);
    g_outTabs = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TABCONTROLA, 0,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_BOTTOM, 0, 0, 700, 26, g_main, (HMENU)232, hInst, 0);
    { TCITEMA ti = {0}; ti.mask = TCIF_TEXT;
      const char* tabs[4] = {"Build", "Message", "Find in Files", "Breakpoints and Tracepoints"};
      for (int i = 0; i < 4; i++) { ti.pszText = (LPSTR)tabs[i]; SendMessageA(g_outTabs, TCM_INSERTITEMA, i, (LPARAM)&ti); }
      SendMessageA(g_outTabs, TCM_SETCURSEL, 1, 0); }
    g_msgs = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "Loaded plugin STK500\r\nLoaded plugin AVR GCC\r\nLoaded partfile: ATmega8.xml\r\nProject is ready and can be saved\r\n",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 700, 140, g_main, (HMENU)203, hInst, 0);
    g_buildEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_CLIPSIBLINGS | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 700, 140, g_main, (HMENU)237, hInst, 0);
    g_findEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_CLIPSIBLINGS | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 700, 140, g_main, (HMENU)238, hInst, 0);
    g_bpList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, 0,
        WS_CHILD | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 700, 140, g_main, (HMENU)239, hInst, 0);
    { LVCOLUMNA lv = {0}; lv.mask = LVCF_TEXT | LVCF_WIDTH; lv.pszText = (LPSTR)"Description"; lv.cx = 420;
      ListView_InsertColumn(g_bpList, 0, &lv);
      lv.pszText = (LPSTR)"Hit Count"; lv.cx = 90; ListView_InsertColumn(g_bpList, 1, &lv); }

    // --- Dock Workspace слева (скрин 7): дерево проекта, вкладки AVR GCC|Processor снизу
    g_wsCap = CreateWindowExA(0, "AVRCLI_Cap", "AVR GCC",
        WS_CHILD | WS_VISIBLE, 0, 0, 230, 20, g_main, (HMENU)225, hInst, 0);
    g_wsTree = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TREEVIEWA, 0,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
        0, 0, 230, 500, g_main, (HMENU)226, hInst, 0);
    g_wsList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, 0,
        WS_CHILD | WS_CLIPSIBLINGS | LVS_REPORT, 0, 0, 230, 500, g_main, (HMENU)228, hInst, 0);
    { LVCOLUMNA lv = {0}; lv.mask = LVCF_TEXT | LVCF_WIDTH; lv.pszText = (LPSTR)"Name"; lv.cx = 120;
      SendMessageA(g_wsList, LVM_INSERTCOLUMNA, 0, (LPARAM)&lv);
      lv.pszText = (LPSTR)"Value"; lv.cx = 90;
      SendMessageA(g_wsList, LVM_INSERTCOLUMNA, 1, (LPARAM)&lv);
      SendMessageA(g_wsList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      addProcRowsOnce(g_wsList); }
    g_wsTabs = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TABCONTROLA, 0,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_BOTTOM, 0, 0, 230, 24, g_main, (HMENU)227, hInst, 0);
    { TCITEMA ti = {0}; ti.mask = TCIF_TEXT;
      ti.pszText = (LPSTR)"AVR GCC"; SendMessageA(g_wsTabs, TCM_INSERTITEMA, 0, (LPARAM)&ti);
      ti.pszText = (LPSTR)"Processor"; SendMessageA(g_wsTabs, TCM_INSERTITEMA, 1, (LPARAM)&ti); }

    // --- Статусбар (скрин 1: CAP NUM OVR/, скрин 7: устройство | платформа | режим)
    g_status = CreateWindowExA(0, STATUSCLASSNAMEA, 0,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g_main, (HMENU)204, hInst, 0);
    int parts[4] = { 300, 550, 700, -1 };
    SendMessageA(g_status, SB_SETPARTS, 4, (LPARAM)parts);
    statusSet(0, "ATmega8");
    statusSet(1, "AVR Simulator 2");
    statusSet(2, "Auto");
    statusSet(3, "CAP NUM OVR");

    EnumChildWindows(g_main, (WNDENUMPROC)[](HWND c, LPARAM) -> BOOL {
        SendMessageA(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE); return TRUE; }, 0);

    msgAppend("Project is ready and can be saved");
    ShowWindow(g_main, nShow);
    if (!g_portableNote.empty()) msgAppend(g_portableNote.c_str());
    UpdateWindow(g_main);

    // акселераторы как в AS4 (подписи в меню): F5 Run, F10 Step Over, F11 Step Into,
    // Shift+F5 Reset, Ctrl+F5 Break
    ACCEL acc[] = {
        { FVIRTKEY,                VK_F5, IDM_DBG_RUN    },
        { FVIRTKEY | FSHIFT,       VK_F5, IDM_DBG_RESET  },
        { FVIRTKEY | FCONTROL,     VK_F5, IDM_DBG_BREAK  },
        { FVIRTKEY,                VK_F10, IDM_DBG_STEPOVER },
        { FVIRTKEY,                VK_F11, IDM_DBG_STEPIN  },
        { FVIRTKEY | FSHIFT,       VK_F11, IDM_DBG_STEPOUT },
    };
    HACCEL hacc = CreateAcceleratorTableA(acc, sizeof(acc)/sizeof(acc[0]));

    // Welcome-диалог при старте (скрин 1); из него же сразу можно открыть недавний проект
    int w = showWelcome(g_main);
    if (w == 1) showWizard(g_main);
    else if (w == 2) openProjectDialog(g_main);
    else if (w == 3 && g_recentName[0]) openProjectAt(g_recentName);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        if (!TranslateAcceleratorA(g_main, hacc, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    DestroyAcceleratorTable(hacc);
    return (int)msg.wParam;
}
