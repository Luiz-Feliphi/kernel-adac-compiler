#include <stdio.h>
#include <stdlib.h>
int main(void) { long n = 5000000; long *c = calloc(n + 1, sizeof(long)); long cnt = 0;
  for (long i = 2; i <= n; i++) if (c[i] == 0) { cnt++; for (long j = i * i; j <= n; j += i) c[j] = 1; }
  printf("%ld\n", cnt); free(c); return 0; }
