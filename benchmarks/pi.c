#include <stdio.h>
int main(void) { double s = 0, sg = 1.0; for (long k = 0; k < 20000000; k++) { s += sg / (2 * k + 1); sg = -sg; } printf("%.6f\n", s * 4); return 0; }
