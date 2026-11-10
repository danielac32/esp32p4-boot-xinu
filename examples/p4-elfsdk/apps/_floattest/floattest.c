/* Prueba de humo: float en apps ELF del P4.  Imprime valores sin %f. */
#include <kernel.h>
#include <stdio.h>
#include <memory.h>
#include <thread.h>
void exit(int c);

extern float floorf(float x);
extern float fabsf(float x);

int main(int argc, char *argv[])
{
    float a = 3.5f, b = 0.5f;
    float s = a + b;
    float p = a * 2.0f;
    float d = p / a;

    (void)argc; (void)argv;
    printf("floattest: 3.5+0.5 = %d\r\n", (int)s);
    printf("floattest: 3.5*2   = %d\r\n", (int)p);
    printf("floattest: 7/3.5   = %d\r\n", (int)d);
    printf("floattest: floorf(5.7)=%d fabsf(-2)=%d\r\n", (int)floorf(5.7f), (int)fabsf(-2.0f));
    printf("floattest: acum 1/3 = %d  0.1+0.2 = %d\r\n",
           (int)(1.0f / 3.0f * 3.0f), (int)(0.1f + 0.2f));
    exit(0);
    return 0;
}
