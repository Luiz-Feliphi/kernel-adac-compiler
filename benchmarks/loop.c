#include <stdio.h>
int main(void) { long soma = 0; for (long i = 0; i < 30000000; i++) soma += (i % 7) * (i & 3); printf("%ld\n", soma); return 0; }
