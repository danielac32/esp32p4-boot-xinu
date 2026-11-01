/**
 * @file audioControl.c
 *
 * Device-level control del codec, de sus relojes y de la salida: volumen,
 * mute, ganancia del micro, arranque/medida del timing I2S y el patron que el
 * AHB-DMA reproduce en bucle.  Ver include/audio.h para los codigos.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <device.h>
#include <audio.h>

/**
 * @ingroup audio
 *
 * @param devptr audio device table entry
 * @param func   command (AUDIO_CTRL_*)
 * @param arg1   command argument
 * @param arg2   command argument
 * @return OK, un valor, o SYSERR para un comando desconocido
 */
devcall audioControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;
    (void)arg2;

    switch (func)
    {
    case AUDIO_CTRL_VOLUME:
        return es8311_set_volume((uint32_t)arg1);

    case AUDIO_CTRL_GET_VOLUME:
        return es8311_get_volume();

    case AUDIO_CTRL_GET_VOLUME_DB:
        return es8311_get_volume_db();

    case AUDIO_CTRL_MUTE:
        return es8311_set_mute(arg1 != 0) ? OK : SYSERR;

    case AUDIO_CTRL_GET_MUTE:
        return es8311_get_mute();

    case AUDIO_CTRL_MIC_GAIN:
        return es8311_set_mic_gain((uint32_t)arg1) ? OK : SYSERR;

    case AUDIO_CTRL_OUT:
    {
        uint32_t applied;

        if (!audio_out_start((uint32_t)arg1, &applied, NULL))
        {
            return SYSERR;
        }
        return (devcall)applied;
    }

    case AUDIO_CTRL_OUT_OFF:
        audio_out_stop();
        return OK;

    case AUDIO_CTRL_OUT_STAT:
        return (devcall)audio_out_status();

    case AUDIO_CTRL_OUT_DESC:
        return (devcall)audio_out_dma_desc();

    case AUDIO_CTRL_OUT_CNT:
        return (devcall)audio_out_sync_count();

    case AUDIO_CTRL_OUT_RING:
        return (devcall)audio_out_ring_frames();

    case AUDIO_CTRL_STREAM:
        if (arg1 != 0)
        {
            return audio_out_stream_start() ? OK : SYSERR;
        }
        audio_out_stop();
        return OK;

    case AUDIO_CTRL_STREAM_WAIT:
        audio_out_stream_drain();
        return OK;

    case AUDIO_CTRL_OUT_QUEUE:
        return (devcall)audio_out_stream_queued();

    case AUDIO_CTRL_OUT_CAP:
        return (devcall)audio_out_stream_capacity();

    case AUDIO_CTRL_IN:
        if (arg1 != 0)
        {
            return audio_in_start() ? OK : SYSERR;
        }
        audio_in_stop();
        return OK;

    case AUDIO_CTRL_IN_STAT:
        return (devcall)audio_in_status();

    case AUDIO_CTRL_IN_DESC:
        return (devcall)audio_in_dma_desc();

    case AUDIO_CTRL_IN_DROP:
        return (devcall)audio_in_dropped();

    case AUDIO_CTRL_IN_RING:
        return (devcall)audio_in_ring_bytes();

    case AUDIO_CTRL_MIC_PGA:
        return es8311_set_mic_pga((uint32_t)arg1) ? OK : SYSERR;

    case AUDIO_CTRL_MIC_GET:
        return es8311_get_mic_pga();

    case AUDIO_CTRL_MIC_STAT:
        return es8311_mic_status();

    case AUDIO_CTRL_MIC_ADCSV:
        return es8311_get_mic_volume();

    case AUDIO_CTRL_MIC_SCALE:
        return es8311_get_mic_scale();

    case AUDIO_CTRL_STREAM_BASE:
        return (devcall)audio_out_stream_base();

    case AUDIO_CTRL_STREAM_GAPS:
        return (devcall)audio_out_stream_gaps();

    case AUDIO_CTRL_IN_BASE:
        return (devcall)audio_in_ring_base();

    case AUDIO_CTRL_IN_EOF:
        return (devcall)audio_in_eof_desc();

    case AUDIO_CTRL_IN_QUEUE:
        return (devcall)audio_in_queued();

    case AUDIO_CTRL_CLOCKS:
        if (arg1 != 0)
        {
            return i2s_clocks_start() ? OK : SYSERR;
        }
        (void)i2s_clocks_stop();
        return OK;

    case AUDIO_CTRL_BCLK:
    {
        uint32_t hz = i2s_bclk_measure((uint32_t)arg1);

        return hz != 0U ? (devcall)hz : SYSERR;
    }

    default:
        return SYSERR;
    }
}
