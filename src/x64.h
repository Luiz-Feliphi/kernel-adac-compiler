/*
 * x64.h - Montador x86-64 minimo: emite bytes de machine code diretamente.
 *
 * There is no external assembler: each a_*() function encodes one instruction
 * (REX, opcode, ModRM/SIB, displacements) into the code buffer.
 */
#ifndef ADAC_X64_H
#define ADAC_X64_H
#include "common.h"

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { XMM0, XMM1 };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };
enum { SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };
enum { SSE_ADD = 0x58, SSE_MUL = 0x59, SSE_SUB = 0x5C, SSE_DIV = 0x5E, SSE_SQRT = 0x51 };

typedef enum { FX_LABEL, FX_DATA, FX_IAT } FixKind;
typedef struct { int pos; FixKind kind; int target; } CodeFix;   /* rel32 a resolver */
typedef struct { int data_off; int label; } DataFix;             /* absolute address in .data; label<0 => points to .data+(-1-label) */

typedef struct {
    uint8_t *code;
    int len, cap;
    int *label_pos;           /* -1 = not bound */
    int nlabels, label_cap;
    CodeFix *fix;
    int nfix, fix_cap;
    DataFix *dfix;
    int ndfix, dfix_cap;
} Asm;

extern Asm A;

int  a_label(void);
void a_bind(int label);
int  a_here(void);
void a_resolve_labels(void);

/* movimentacao */
void a_mov_rr(int dst, int src);
void a_mov_ri(int r, int64_t v);
void a_load(int dst, int base, int32_t disp);        /* mov dst,[base+disp]  */
void a_store(int base, int32_t disp, int src);       /* mov [base+disp],src  */
void a_load8(int dst, int base, int32_t disp);       /* movzx dst, byte [..] */
void a_store8(int base, int32_t disp, int src);
void a_lea(int dst, int base, int32_t disp);
void a_lea_label(int dst, int label);               /* lea dst,[rip+label] */
void a_lea_data(int dst, int data_off);              /* lea dst,[rip+.data+off] */
void a_load_data(int dst, int data_off);
void a_store_data(int data_off, int src);
void a_movzx_r8(int dst, int src);
/* aritmetica */
void a_alu_rr(int op, int dst, int src);
void a_alu_ri(int op, int dst, int32_t imm);
void a_alu_rm(int op, int reg, int base, int32_t disp);   /* op reg,[base+disp] */
void a_imul_rr(int dst, int src);
void a_cqo(void);
void a_idiv(int r);
void a_neg(int r);
void a_not(int r);
void a_shift_cl(int kind, int r);
void a_test_rr(int a, int b);
void a_setcc(int cc, int r);
void a_and8_al_cl(void);
void a_or8_al_cl(void);
/* stack and control flow */
void a_push(int r);
void a_pop(int r);
void a_ret(void);
void a_leave(void);
void a_int3(void);
void a_jmp(int label);
void a_jcc(int cc, int label);
void a_call(int label);
void a_call_mem(int base, int32_t disp);
void a_call_reg(int r);
void a_jmp_mem(int base, int32_t disp);
void a_call_iat(int idx);
int  a_sub_rsp_patchable(void);                      /* returns the position of the imm32 */
void a_patch32(int pos, int32_t v);
/* SSE2 (double) */
void a_movq_xr(int xmm, int r);
void a_movq_rx(int r, int xmm);
void a_sse(int op, int dst, int src);
void a_ucomisd(int x1, int x2);
void a_cvtsi2sd(int xmm, int r);
void a_cvttsd2si(int r, int xmm);
/* strings */
void a_rep_movsb(void);
/* data */
void a_data_fix_func(int data_off, int label);

#endif
