// gui/emu_api.h - тонкий слой доступа к ядру эмулятора emu.cpp,
// скомпилированному в GUI-процесс (define EMU_LIB на обеих единицах).
// Всё ядро - file-scope static - синглтон в процессе; API даёт безопасные глаголы.
#pragma once
#include <cstdint>
#include <string>

void     emuapi_sink(void (*fn)(const char* s));       // направить весь вывод в GUI
bool     emuapi_init(const char* root, const char* elf); // root: echo.ctos + Procs/; elf: путь программы
bool     emuapi_load();                                 // загрузить ELF в flash
bool     emuapi_loaded();
void     emuapi_stop();                                 // g_running=false (вызывается из другого потока - безопасно)
bool     emuapi_stepInstrs(int n);                      // n машинных шагов без dump'а dash
int      emuapi_run();                                  // прогон до брейпойнта/лимита (блокирующий, из worker-потока)
void     emuapi_reset();
bool     emuapi_cmd(const char* line);   // "next"/"finish" (C-линии из DWARF), "io SREG" и пр.
bool     emuapi_findReg(const char* name, int* sramAddr); // имя из профиля -> SRAM-адрес (0x20+io)
uint8_t  emuapi_readSram(int a);                        // адрес SRAM (учитывает SREG/UDR/pins)
void     emuapi_writeIo(const char* name, int val);
uint32_t emuapi_pc();
uint64_t emuapi_cycles();
uint8_t  emuapi_sreg();
uint16_t emuapi_sp();
const char* emuapi_chip();
unsigned    emuapi_fcpu();
std::string emuapi_tioText();                           // Terminal I/O: собранное UART-TX
