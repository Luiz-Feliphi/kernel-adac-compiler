/*
 * pe.c - Writes a PE32+ (Windows x64) console executable.
 *
 * File layout:
 *   0x000  DOS header (64 bytes, e_lfanew = 0x40)
 *   0x040  PE signature, COFF header, PE32+ optional header, section table
 *   0x200  .text   (RVA 0x1000)   machine code
 *          .data   (RVA alinhado) globals, vtables, strings    (leitura/escrita)
 *          .idata  (RVA alinhado) tabela de importacao de KERNEL32.DLL
 *
 * The image uses a fixed base (0x140000000) and no relocations, so absolute
 * addresses (e.g. function pointers in vtables) are resolved here.
 */
#include "pe.h"
#include "x64.h"

#define IMAGE_BASE   0x140000000ULL
#define SECT_ALIGN   0x1000
#define FILE_ALIGN   0x200
#define TEXT_RVA     0x1000

static const char *IMPORT_NAMES[API__COUNT] = {
    "ExitProcess", "GetStdHandle", "WriteFile", "ReadFile",
    "GetProcessHeap", "HeapAlloc", "HeapFree", "SetConsoleOutputCP"
};

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

typedef struct { uint8_t *b; int len; } Out;
static void put(Out *o, int off, uint64_t v, int n) {
    for (int i = 0; i < n; i++) o->b[off + i] = (uint8_t)(v >> (8 * i));
}

void pe_write(const char *path, uint8_t *data, int data_len, int entry_label) {
    a_resolve_labels();

    uint32_t text_len = (uint32_t)A.len;
    uint32_t text_rva = TEXT_RVA;
    uint32_t text_raw = align_up(text_len, FILE_ALIGN);
    uint32_t data_rva = align_up(text_rva + text_len, SECT_ALIGN);
    uint32_t data_vsz = (uint32_t)(data_len > 0 ? data_len : 8);
    uint32_t data_raw = align_up(data_vsz, FILE_ALIGN);
    uint32_t idata_rva = align_up(data_rva + data_vsz, SECT_ALIGN);

    /* ----- build .idata ----- */
    const int N = API__COUNT;
    uint32_t idt_off = 0;                           /* 2 descritores de 20 bytes */
    uint32_t ilt_off = 40;
    uint32_t iat_off = ilt_off + (uint32_t)(N + 1) * 8;
    uint32_t names_off = iat_off + (uint32_t)(N + 1) * 8;
    uint32_t hint_off[API__COUNT];
    uint32_t cur = names_off;
    for (int i = 0; i < N; i++) {
        hint_off[i] = cur;
        cur += 2 + (uint32_t)strlen(IMPORT_NAMES[i]) + 1;
        cur = align_up(cur, 2);
    }
    uint32_t dll_off = cur;
    cur += (uint32_t)strlen("KERNEL32.dll") + 1;
    uint32_t idata_len = cur;
    uint32_t idata_raw = align_up(idata_len, FILE_ALIGN);
    uint8_t *idata = xcalloc(idata_raw);
    Out io = {idata, (int)idata_raw};
    put(&io, (int)(idt_off + 0), idata_rva + ilt_off, 4);   /* OriginalFirstThunk */
    put(&io, (int)(idt_off + 12), idata_rva + dll_off, 4);  /* Name */
    put(&io, (int)(idt_off + 16), idata_rva + iat_off, 4);  /* FirstThunk */
    for (int i = 0; i < N; i++) {
        put(&io, (int)(ilt_off + (uint32_t)i * 8), idata_rva + hint_off[i], 8);
        put(&io, (int)(iat_off + (uint32_t)i * 8), idata_rva + hint_off[i], 8);
        memcpy(idata + hint_off[i] + 2, IMPORT_NAMES[i], strlen(IMPORT_NAMES[i]));
    }
    memcpy(idata + dll_off, "KERNEL32.dll", strlen("KERNEL32.dll"));

    /* ----- resolve code fixups ----- */
    for (int i = 0; i < A.nfix; i++) {
        CodeFix *f = &A.fix[i];
        uint32_t next = text_rva + (uint32_t)f->pos + 4;
        if (f->kind == FX_DATA) a_patch32(f->pos, (int32_t)(data_rva + (uint32_t)f->target - next));
        else if (f->kind == FX_IAT) a_patch32(f->pos, (int32_t)(idata_rva + iat_off + (uint32_t)f->target * 8 - next));
    }
    /* ----- absolute function addresses in .data ----- */
    for (int i = 0; i < A.ndfix; i++) {
        uint64_t addr = A.dfix[i].label < 0
            ? IMAGE_BASE + data_rva + (uint32_t)(-1 - A.dfix[i].label)
            : IMAGE_BASE + text_rva + (uint32_t)A.label_pos[A.dfix[i].label];
        for (int k = 0; k < 8; k++) data[A.dfix[i].data_off + k] = (uint8_t)(addr >> (8 * k));
    }

    /* ----- cabecalhos ----- */
    uint32_t headers = 0x200;
    uint32_t file_size = headers + text_raw + data_raw + idata_raw;
    uint32_t image_size = idata_rva + align_up(idata_len, SECT_ALIGN);
    uint8_t *img = xcalloc(file_size);
    Out o = {img, (int)file_size};

    img[0] = 'M'; img[1] = 'Z';
    put(&o, 0x3C, 0x40, 4);                          /* e_lfanew */
    int pe = 0x40;
    memcpy(img + pe, "PE\0\0", 4);
    int coff = pe + 4;
    put(&o, coff + 0, 0x8664, 2);                    /* Machine: AMD64 */
    put(&o, coff + 2, 3, 2);                         /* NumberOfSections */
    put(&o, coff + 16, 0xF0, 2);                     /* SizeOfOptionalHeader */
    put(&o, coff + 18, 0x0023, 2);                   /* EXECUTABLE | LARGE_ADDRESS_AWARE | RELOCS_STRIPPED */
    int opt = coff + 20;
    put(&o, opt + 0, 0x20B, 2);                      /* PE32+ */
    put(&o, opt + 2, 0x0001, 2);                     /* versao do linker */
    put(&o, opt + 4, text_raw, 4);                   /* SizeOfCode */
    put(&o, opt + 8, data_raw + idata_raw, 4);       /* SizeOfInitializedData */
    put(&o, opt + 16, text_rva + (uint32_t)A.label_pos[entry_label], 4);   /* AddressOfEntryPoint */
    put(&o, opt + 20, text_rva, 4);                  /* BaseOfCode */
    put(&o, opt + 24, IMAGE_BASE, 8);
    put(&o, opt + 32, SECT_ALIGN, 4);
    put(&o, opt + 36, FILE_ALIGN, 4);
    put(&o, opt + 40, 6, 2);                         /* OS version 6.0 */
    put(&o, opt + 48, 6, 2);                         /* Subsystem version 6.0 */
    put(&o, opt + 56, image_size, 4);
    put(&o, opt + 60, headers, 4);
    put(&o, opt + 68, 3, 2);                         /* Subsystem: CONSOLE */
    put(&o, opt + 70, 0x0100, 2);                    /* NX_COMPAT (no ASLR: fixed base) */
    put(&o, opt + 72, 0x800000, 8);                  /* stack reserve 8 MB */
    put(&o, opt + 80, 0x10000, 8);                   /* stack commit */
    put(&o, opt + 88, 0x100000, 8);                  /* heap reserve */
    put(&o, opt + 96, 0x1000, 8);                    /* heap commit */
    put(&o, opt + 108, 16, 4);                       /* NumberOfRvaAndSizes */
    int dd = opt + 112;
    put(&o, dd + 1 * 8, idata_rva + idt_off, 4);     /* diretorio de importacao */
    put(&o, dd + 1 * 8 + 4, 40, 4);
    put(&o, dd + 12 * 8, idata_rva + iat_off, 4);    /* IAT */
    put(&o, dd + 12 * 8 + 4, (uint32_t)(N + 1) * 8, 4);

    /* section table */
    int sh = opt + 0xF0;
    struct { const char *name; uint32_t vsz, rva, raw, rawoff, flags; } secs[3] = {
        {".text",  text_len ? text_len : 1, text_rva, text_raw, headers, 0x60000020u},
        {".data",  data_vsz, data_rva, data_raw, headers + text_raw, 0xC0000040u},
        {".idata", idata_len, idata_rva, idata_raw, headers + text_raw + data_raw, 0xC0000040u},
    };
    for (int i = 0; i < 3; i++) {
        int s = sh + i * 40;
        memcpy(img + s, secs[i].name, strlen(secs[i].name));
        put(&o, s + 8, secs[i].vsz, 4);
        put(&o, s + 12, secs[i].rva, 4);
        put(&o, s + 16, secs[i].raw, 4);
        put(&o, s + 20, secs[i].rawoff, 4);
        put(&o, s + 36, secs[i].flags, 4);
    }

    memcpy(img + headers, A.code, text_len);
    if (data_len > 0) memcpy(img + headers + text_raw, data, (size_t)data_len);
    memcpy(img + headers + text_raw + data_raw, idata, idata_len);

    FILE *f = fopen(path, "wb");
    if (!f) diag_fatal("could not create '%s'", path);
    if (fwrite(img, 1, file_size, f) != file_size) diag_fatal("error writing '%s'", path);
    fclose(f);
#ifndef _WIN32
    { extern int chmod(const char *, unsigned int); chmod(path, 0755); }
#endif
    free(img);
    free(idata);
}
