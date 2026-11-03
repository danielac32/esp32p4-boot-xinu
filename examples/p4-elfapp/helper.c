/* helper.c -- segunda unidad de traduccion del app.
 *
 * Existe solo para que el enlazado parcial (ld -r) tenga que dejar una
 * relocacion REAL contra un simbolo que esta en OTRO .o: 'main' llama a
 * 'helper_sum', que esta en este fichero.  Con un solo fichero no se prueba el camino que interesa, que es
 * el de los simbolos sin resolver y sus HI20/LO12 y CALL.
 */

int helper_sum(const int *v, int n)
{
    int s = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        s += v[i];
    }

    return s;
}
