/*
 * x64.c - Codificacao de instrucoes x86-64.
 *
 * Formato geral:  [prefixo obrigatorio] [REX] opcode [ModRM] [SIB] [disp] [imm]
 *   REX = 0100WRXB   (W: 64 bits, R: estende reg, X: indice, B: estende rm/base)
 *   ModRM = mod(2) reg(3) rm(3)
 */
#include "x64.h"

Asm A;

static void b8(uint8_t v) {
    if (A.len == A.cap) { A.cap = A.cap ? A.cap * 2 : 4096; A.code = xrealloc(A.code, (size_t)A.cap); }
    A.code[A.len++] = v;
}
static void b32(uint32_t v) { for (int i = 0; i < 4; i++) b8((uint8_t)(v >> (8 * i))); }
static void b64(uint64_t v) { for (int i = 0; i < 8; i++) b8((uint8_t)(v >> (8 * i))); }

/* ---------- labels e fixups ---------- */
int a_label(void) {
    if (A.nlabels == A.label_cap) {
        A.label_cap = A.label_cap ? A.label_cap * 2 : 256;
        A.label_pos = xrealloc(A.label_pos, sizeof(int) * (size_t)A.label_cap);
    }
    A.label_pos[A.nlabels] = -1;
    return A.nlabels++;
}
void a_bind(int l) { A.label_pos[l] = A.len; }
int  a_here(void) { return A.len; }

static void add_fix(FixKind k, int target) {
    if (A.nfix == A.fix_cap) { A.fix_cap = A.fix_cap ? A.fix_cap * 2 : 1024; A.fix = xrealloc(A.fix, sizeof(CodeFix) * (size_t)A.fix_cap); }
    A.fix[A.nfix++] = (CodeFix){A.len, k, target};
    b32(0);   /* reserva rel32 */
}

void a_data_fix_func(int data_off, int label) {
    if (A.ndfix == A.dfix_cap) { A.dfix_cap = A.dfix_cap ? A.dfix_cap * 2 : 64; A.dfix = xrealloc(A.dfix, sizeof(DataFix) * (size_t)A.dfix_cap); }
    A.dfix[A.ndfix++] = (DataFix){data_off, label};
}

void a_patch32(int pos, int32_t v) {
    for (int i = 0; i < 4; i++) A.code[pos + i] = (uint8_t)((uint32_t)v >> (8 * i));
}

void a_resolve_labels(void) {
    for (int i = 0; i < A.nfix; i++) {
        if (A.fix[i].kind != FX_LABEL) continue;
        int target = A.label_pos[A.fix[i].target];
        if (target < 0) diag_fatal("internal error: label %d is not bound", A.fix[i].target);
        a_patch32(A.fix[i].pos, target - (A.fix[i].pos + 4));
    }
}

/* ---------- prefixos e ModRM ---------- */
static void rex(int w, int reg, int rm, bool force) {
    uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((rm >> 3) & 1));
    if (r != 0x40 || force) b8(r);
}
static void modrm_rr(int reg, int rm) { b8((uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7))); }

/* [base + disp] with SIB when base = rsp/r12 and disp8/disp32 as needed */
static void modrm_mem(int reg, int base, int32_t disp) {
    int rm = base & 7;
    int mod;
    if (disp == 0 && rm != 5) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    b8((uint8_t)((mod << 6) | ((reg & 7) << 3) | rm));
    if (rm == 4) b8(0x24);
    if (mod == 1) b8((uint8_t)disp);
    else if (mod == 2) b32((uint32_t)disp);
}

/* ---------- movimentacao ---------- */
void a_mov_rr(int dst, int src) { rex(1, src, dst, false); b8(0x89); modrm_rr(src, dst); }

void a_mov_ri(int r, int64_t v) {
    if (v >= 0 && v <= 0xFFFFFFFFLL) {            /* mov r32, imm32 (zera os 32 altos) */
        rex(0, 0, r, false); b8((uint8_t)(0xB8 + (r & 7))); b32((uint32_t)v);
    } else if (v >= INT32_MIN && v < 0) {          /* mov r64, simm32 */
        rex(1, 0, r, false); b8(0xC7); modrm_rr(0, r); b32((uint32_t)(int32_t)v);
    } else {                                       /* movabs */
        rex(1, 0, r, false); b8((uint8_t)(0xB8 + (r & 7))); b64((uint64_t)v);
    }
}

void a_load(int dst, int base, int32_t disp) { rex(1, dst, base, false); b8(0x8B); modrm_mem(dst, base, disp); }
void a_store(int base, int32_t disp, int src) { rex(1, src, base, false); b8(0x89); modrm_mem(src, base, disp); }
void a_load8(int dst, int base, int32_t disp) { rex(1, dst, base, false); b8(0x0F); b8(0xB6); modrm_mem(dst, base, disp); }
void a_store8(int base, int32_t disp, int src) { rex(0, src, base, true); b8(0x88); modrm_mem(src, base, disp); }
void a_lea(int dst, int base, int32_t disp) { rex(1, dst, base, false); b8(0x8D); modrm_mem(dst, base, disp); }

static void rip_op(uint8_t opcode, int reg, int data_off) {
    rex(1, reg, 0, false);
    b8(opcode);
    b8((uint8_t)(((reg & 7) << 3) | 5));
    add_fix(FX_DATA, data_off);
}
void a_lea_label(int dst, int label) {
    rex(1, dst, 0, false); b8(0x8D); b8((uint8_t)(((dst & 7) << 3) | 5));
    add_fix(FX_LABEL, label);
}
void a_lea_data(int dst, int off) { rip_op(0x8D, dst, off); }
void a_load_data(int dst, int off) { rip_op(0x8B, dst, off); }
void a_store_data(int off, int src) { rip_op(0x89, src, off); }

void a_movzx_r8(int dst, int src) { rex(1, dst, src, true); b8(0x0F); b8(0xB6); modrm_rr(dst, src); }

/* ---------- aritmetica ---------- */
void a_alu_rr(int op, int dst, int src) { rex(1, src, dst, false); b8((uint8_t)((op << 3) | 1)); modrm_rr(src, dst); }
void a_alu_ri(int op, int dst, int32_t imm) {
    rex(1, 0, dst, false);
    if (imm >= -128 && imm <= 127) { b8(0x83); modrm_rr(op, dst); b8((uint8_t)imm); }
    else { b8(0x81); modrm_rr(op, dst); b32((uint32_t)imm); }
}
void a_alu_rm(int op, int reg, int base, int32_t disp) { rex(1, reg, base, false); b8((uint8_t)((op << 3) | 3)); modrm_mem(reg, base, disp); }
void a_imul_rr(int dst, int src) { rex(1, dst, src, false); b8(0x0F); b8(0xAF); modrm_rr(dst, src); }
void a_cqo(void) { b8(0x48); b8(0x99); }
void a_idiv(int r) { rex(1, 0, r, false); b8(0xF7); modrm_rr(7, r); }
void a_neg(int r) { rex(1, 0, r, false); b8(0xF7); modrm_rr(3, r); }
void a_not(int r) { rex(1, 0, r, false); b8(0xF7); modrm_rr(2, r); }
void a_shift_cl(int kind, int r) { rex(1, 0, r, false); b8(0xD3); modrm_rr(kind, r); }
void a_test_rr(int a, int b) { rex(1, b, a, false); b8(0x85); modrm_rr(b, a); }
void a_setcc(int cc, int r) { rex(0, 0, r, false); b8(0x0F); b8((uint8_t)(0x90 + cc)); modrm_rr(0, r); }
void a_and8_al_cl(void) { b8(0x20); b8(0xC8); }
void a_or8_al_cl(void) { b8(0x08); b8(0xC8); }

/* ---------- stack and control flow ---------- */
void a_push(int r) { rex(0, 0, r, false); b8((uint8_t)(0x50 + (r & 7))); }
void a_pop(int r) { rex(0, 0, r, false); b8((uint8_t)(0x58 + (r & 7))); }
void a_ret(void) { b8(0xC3); }
void a_leave(void) { b8(0xC9); }
void a_int3(void) { b8(0xCC); }
void a_jmp(int l) { b8(0xE9); add_fix(FX_LABEL, l); }
void a_jcc(int cc, int l) { b8(0x0F); b8((uint8_t)(0x80 + cc)); add_fix(FX_LABEL, l); }
void a_call(int l) { b8(0xE8); add_fix(FX_LABEL, l); }
void a_call_mem(int base, int32_t disp) { rex(0, 0, base, false); b8(0xFF); modrm_mem(2, base, disp); }
void a_call_reg(int r) { rex(0, 0, r, false); b8(0xFF); modrm_rr(2, r); }
void a_jmp_mem(int base, int32_t disp) { rex(0, 0, base, false); b8(0xFF); modrm_mem(4, base, disp); }
void a_call_iat(int idx) { b8(0xFF); b8(0x15); add_fix(FX_IAT, idx); }
int  a_sub_rsp_patchable(void) {
    rex(1, 0, RSP, false); b8(0x81); modrm_rr(ALU_SUB, RSP);
    int pos = A.len;
    b32(0);
    return pos;
}

/* ---------- SSE2 ---------- */
void a_movq_xr(int x, int r) { b8(0x66); rex(1, x, r, false); b8(0x0F); b8(0x6E); modrm_rr(x, r); }
void a_movq_rx(int r, int x) { b8(0x66); rex(1, x, r, false); b8(0x0F); b8(0x7E); modrm_rr(x, r); }
void a_sse(int op, int dst, int src) { b8(0xF2); b8(0x0F); b8((uint8_t)op); modrm_rr(dst, src); }
void a_ucomisd(int x1, int x2) { b8(0x66); b8(0x0F); b8(0x2E); modrm_rr(x1, x2); }
void a_cvtsi2sd(int x, int r) { b8(0xF2); rex(1, x, r, false); b8(0x0F); b8(0x2A); modrm_rr(x, r); }
void a_cvttsd2si(int r, int x) { b8(0xF2); rex(1, r, x, false); b8(0x0F); b8(0x2C); modrm_rr(r, x); }

void a_rep_movsb(void) { b8(0xF3); b8(0xA4); }
