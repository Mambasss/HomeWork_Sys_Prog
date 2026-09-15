#include <stdio.h>
#include "sigfpe.h"

volatile int b = 0;

int main(void)
{
    install_sigfpe_handler();

    volatile int a = 100;
    int r = a / b;            /* делитель — volatile, компилятор обязан читать */

    printf("[A] after division: r = %d\n", r);

    if (b == 0)               /* volatile → перечитывает b из памяти */
        printf("[A] b is 0\n");
    else
        printf("[A] b is NOT 0\n");
    return 0;
}