/* helper.c -- segunda unidad de traduccion del app.
 *
 * Existe solo para que el enlazado parcial (ld -r) tenga que dejar una
 * relocacion REAL contra un simbolo que esta en OTRO .o: 'main' llama a
 * 'helper_media' y 'helper_minmax', que estan en este fichero y en otra seccion
 * .text.  Con un solo fichero no se prueba el camino que interesa, que es el de
 * los simbolos sin resolver y sus HI20/LO12 y CALL.
 */

int helper_media(const int *v, int n)
{
    int total = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        total += v[i];
    }

    return total / n;
}

/* Escribe por los punteros que le pasa el llamador: llegan de la pila del app,
 * asi que la direccion no lleva relocacion ninguna y el prueba-que-prueba es que
 * el dato recorre las dos unidades de traduccion sin perderse. */
int helper_minmax(const int *v, int n, int *min, int *max)
{
    int i;

    *min = *max = v[0];

    for (i = 1; i < n; i++)
    {
        if (v[i] < *min)
        {
            *min = v[i];
        }
        else if (v[i] > *max)
        {
            *max = v[i];
        }
    }

    return n;
}
