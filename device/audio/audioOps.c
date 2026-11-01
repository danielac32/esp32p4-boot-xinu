/**
 * @file audioOps.c
 *
 * read() y write() del device `audio`: el PCM entra y sale por aqui en el
 * formato del cable, tramas estereofonicas de dos slots de 16 bits a 48 kHz
 * (4 bytes por trama).  Nada de cabeceras ni de decodificacion: eso es tarea
 * de quien llama (shell/xsh_audio.c pone el WAV en este formato antes de
 * escribir).
 *
 * El write apila tramas en el anillo de PSRAM de audioOut.c y duerme cuando se
 * llena, que es lo que sostiene el ritmo de 192000 bytes por segundo sin
 * interrupciones ni colas propias.  El read vacia el anillo de captura de
 * audioIn.c y no espera: devuelve lo que habia, 0 si el motor aun no ha llenado
 * ningun trozo.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <kernel.h>
#include <audio.h>

/**
 * @ingroup audio
 *
 * @param devptr audio device table entry
 * @param buf    tramas de 16 bits estereo
 * @param count  bytes de @p buf a reproducir
 * @return bytes aceptados, o SYSERR si la salida no se puede montar
 */
devcall audioWrite(device *devptr, const void *buf, uint32 count)
{
    uint32_t sent;

    (void)devptr;

    if (0U == count || NULL == buf)
    {
        return 0;
    }

    /* Escribir sin salida montada es lo normal en el primer write de un
     * fichero: se monta el stream alli mismo.  Si estaba sonando un tono,
     * audio_out_stream_start() lo baja con la misma secuencia con la que se
     * subio. */
    if (!audio_out_streaming() && !audio_out_stream_start())
    {
        return SYSERR;
    }

    sent = audio_out_stream_write(buf, count);
    return (devcall)sent;
}

/**
 * @ingroup audio
 *
 * @param devptr audio device table entry
 * @param buf    donde se vuelca lo capturado
 * @param count  bytes maximos a entregar
 * @return bytes entregados (multiplo de la trama), o SYSERR si la captura no
 *         se puede montar
 */
devcall audioRead(device *devptr, void *buf, uint32 count)
{
    (void)devptr;

    if (0U == count || NULL == buf)
    {
        return 0;
    }

    if (!audio_in_running() && !audio_in_start())
    {
        return SYSERR;
    }

    return (devcall)audio_in_read(buf, count);
}
