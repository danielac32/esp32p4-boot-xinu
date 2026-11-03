/*
 * sum.c -- el blob de ejemplo.
 *
 * Funcion HOJA entera: no llama a nada, no toca memoria, no usa punto
 * flotante.  Por eso no genera GOT ni ninguna relocation: el binario plano
 * que sale de aqui es independiente de donde lo cargues, que es exactamente
 * lo que se necesita para saltar a el desde PSRAM.
 *
 * Se compila con las mismas marchas del proyecto (platformVars):
 *   -march=rv32imafc_zicsr_zifencei -mabi=ilp32f
 *
 * La entrada va en su propia seccion (.text.entry) para que el volcado plano la
 * ponga en el offset 0; ver sqsum.c, que aplica la misma regla.
 */

__attribute__((section(".text.entry")))
int sum(int a, int b)
{
    return a + b;
}
