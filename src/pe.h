/*
 * pe.h - Windows executable generator (PE32+ / x64).
 */
#ifndef ADAC_PE_H
#define ADAC_PE_H
#include "common.h"

/* Functions imported from KERNEL32.DLL, in the order used by __winapi(n, ...) */
enum {
    API_ExitProcess, API_GetStdHandle, API_WriteFile, API_ReadFile,
    API_GetProcessHeap, API_HeapAlloc, API_HeapFree, API_SetConsoleOutputCP,
    API__COUNT
};

/*
 * Resolves code (A) and data fixups, computes the layout and writes the .exe.
 *  data / data_len : contents of the .data section (globals + vtables + strings)
 *  entry_label     : label of the entry point in .text
 */
void pe_write(const char *path, uint8_t *data, int data_len, int entry_label);

#endif
