// AVR CLI Studio - GUI (Win32, чистый нативный API; портативный один exe)
// Стиль и расположение копируют AVR Studio 4 по скринов Pictures/1..25.png.
// Этап M1 (каркас): главное окно, меню AS4, статусбар, Welcome-диалог,
// правый док I/O View (модули -> регистры -> биты), нижний Message с табами.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
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
static HWND g_tbStd = 0, g_tbDbg = 0, g_tabMsg = 0, g_editMain = 0;
static HFONT g_fontUi = 0;

static void tbAddStr(HWND tb, int id, const char* text, int sep) {
    TBBUTTON b = {0};
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
static std::string g_projRoot;                     // корень проекта (echo.ctos + Procs/)
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

// worker-поток прогона (F5): завершение уведомляет главное окно
static DWORD WINAPI runThreadProc(LPVOID) {
    emuapi_run();
    g_runBusy = false;
    if (g_main) PostMessageA(g_main, WM_APP + 1, 0, 0);
    return 0;
}

// старт отладки: инициализация ядра + загрузка ELF
static void startDebugging() {
    if (g_runBusy) { msgAppend("-- Прогон уже работает (Break = Ctrl+F5)"); return; }
    if (!g_coreInit) {
        // корень проекта: GUI exe живёт в <root>/gui, значит root = его родитель.
        // (в финальной портативной сборке будет unpack-каталог)
        char exePath[MAX_PATH]; GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string d = exePath;
        size_t sl = d.find_last_of("\\/");         d = d.substr(0, sl);             // снимаем имя exe -> /gui
        sl = d.find_last_of("\\/");  if (sl != std::string::npos) g_projRoot = d.substr(0, sl); else g_projRoot = ".";
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

// ------------------------------------------------------------- Welcome box
// Скрин 1: "Welcome to AVR Studio 4": New Project, Open, Recent projects.
static INT_PTR CALLBACK DlgWelcome(HWND h, UINT m, WPARAM w, LPARAM) {
    switch (m) {
        case WM_INITDIALOG: {
            RECT r; GetWindowRect(GetParent(h) ? GetParent(h) : h, &r); (void)r;
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDCANCEL || LOWORD(w) == 2) { EndDialog(h, 0); return TRUE; }
            if (LOWORD(w) == 101) { EndDialog(h, 1); return TRUE; }      // New Project
            if (LOWORD(w) == 102) { EndDialog(h, 2); return TRUE; }      // Open
            break;
    }
    return FALSE;
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
            int rightW  = 240;   // I/O View док
            int bottomH = 170;   // Message панель
            int workH = cy - yTop - statusH;
            int bottomY = cy - statusH - bottomH;
            // центральный редактор
            MoveWindow(g_editMain, 0, yTop, cx - rightW, workH - bottomH, TRUE);
            // правый док: сверху модули, снизу регистры
            MoveWindow(g_ioTree, cx - rightW, yTop, rightW, (workH - bottomH) / 2, TRUE);
            MoveWindow(g_ioList, cx - rightW, yTop + (workH - bottomH) / 2, rightW, (workH - bottomH) / 2, TRUE);
            // нижняя Message-панель: таб + вывод (скрин 15: Build/Message/Find in Files/Breakpoints)
            MoveWindow(g_tabMsg, 0, bottomY, cx - rightW, bottomH, TRUE);
            MoveWindow(g_msgs, 3, 26, cx - rightW - 6, bottomH - 30, TRUE);
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
                    if (g_coreInit) { emuapi_reset(); refreshIoListView(); }
                    return 0;
                case IDM_DBG_STEPIN:     // Step Into (F11) = машинный шаг
                    if (!g_coreInit || !emuapi_loaded()) { msgAppend("! Сначала Debug > Start Debugging"); return 0; }
                    if (emuapi_stepInstrs(1)) { refreshIoListView(); }
                    return 0;
                case IDM_DBG_STEPOVER:   // Step Over (F10) = C-строка over (DWARF)
                    if (!g_coreInit || !emuapi_loaded()) { msgAppend("! Сначала Debug > Start Debugging"); return 0; }
                    emuapi_cmd("next");
                    refreshIoListView();
                    return 0;
                case IDM_DBG_STEPOUT:    // Step Out (Shift+F11)
                    if (!g_coreInit || !emuapi_loaded()) { msgAppend("! Сначала Debug > Start Debugging"); return 0; }
                    emuapi_cmd("finish");
                    refreshIoListView();
                    return 0;
                case IDM_DBG_STOP:       // Stop Debugging
                    emuapi_stop();
                    msgAppend("-- Debugging stopped");
                    return 0;
            }
            msgAppend((std::string("[menu] id=") + std::to_string(id)).c_str());
            return 0;
        }
        case WM_APP + 1:  // прогон завершён (из worker-потока)
            refreshIoListView();
            msgAppend("-- Run ended");
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetUnhandledExceptionFilter(crashFilter);
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
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mFile,  "&File");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mProj,  "&Project");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mBuild, "&Build");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mView,  "&View");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mTools, "&Tools");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mDbg,   "&Debug");
        AppendMenuA(mb, MF_POPUP, (UINT_PTR)mHelp,  "&Help");
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

    // --- Message панель (нижний док, скрин 15): таб + вывод
    g_tabMsg = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TABCONTROLA, 0,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 700, 170, g_main, (HMENU)206, hInst, 0);
    { TCITEMA ti = {0}; ti.mask = TCIF_TEXT;
      const char* tabs[4] = {"Build", "Message", "Find in Files", "Breakpoints and Tracepoints"};
      for (int i = 0; i < 4; i++) { ti.pszText = (LPSTR)tabs[i]; SendMessageA(g_tabMsg, TCM_INSERTITEMA, i, (LPARAM)&ti); }
      SendMessageA(g_tabMsg, TCM_SETCURSEL, 1, 0); }
    g_msgs = CreateWindowExA(0, "EDIT", "Loaded plugin STK500\r\nLoaded plugin AVR GCC\r\nLoaded partfile: ATmega8.xml\r\n",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        3, 26, 700, 140, g_tabMsg, (HMENU)203, hInst, 0);

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
