#include <stdio.h>
#include <stdlib.h>
typedef struct P { void *vt; long v; } P;
int main(void) { long soma = 0;
  for (long i = 0; i < 3000000; i++) { P *p = calloc(1, sizeof(P)); p->v = i; soma += p->v & 7; free(p); }
  printf("%ld\n", soma); return 0; }
