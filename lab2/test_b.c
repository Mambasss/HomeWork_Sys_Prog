#include <stdio.h>
#include "sigfpe.h"

volatile int bv = 0;

int main(void)
{
    install_sigfpe_handler();

    int a = 100;
    int b = bv;               /* volatile-чтение → обычная локальная переменная */
    int r = a / b;            /* b==0 — это UB. Компилятор вправе считать b != 0 */

    printf("[B] after division: r = %d\n", r);

    if (b == 0)               /* с точки зрения компилятора — всегда false */
        printf("[B] b is 0\n");
    else
        printf("[B] b is NOT 0\n");
    return 0;
}