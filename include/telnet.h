/**
 * @file     telnet.h
 *
 * Servidor telnet sobre lwIP (raw API).  Un device TELNETn por cliente hace de
 * "descriptor": el shell de cada cliente lee/escribe ahí y sus mensajes viajan
 * por su conexión TCP.
 *
 * Modelo (igual que ping/http client): la raw API de lwIP NO es reentrante, así
 * que toda la parte de red (accept, recv, flush, IAC) la ejecuta el NET task
 * dentro de telnet_service().  Los hilos shell sólo tocan buffers y semáforos:
 *
 *   NET task  --push bytes-->  in[]   --wait(isem)-->  telnetRead()  (shell)
 *   shell     --telnetWrite--> out[]  --flush-->       NET task      --> TCP
 *
 * El device hace el "cooking" de terminal (eco, backspace, CR/LF) y filtra los
 * comandos IAC de telnet.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _TELNET_H_
#define _TELNET_H_

#include <conf.h>
#include <device.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stddef.h>

/* Puerto por defecto */
#define TELNET_PORT     23

/* Tamaños de buffer (por conexión) */
#ifndef TELNET_RXBUF
#define TELNET_RXBUF    1024    /**< bytes de red -> shell                  */
#endif
#ifndef TELNET_OBLEN
#define TELNET_OBLEN    1024    /**< bytes shell -> red                     */
#endif

/* Pila del hilo shell de cada cliente */
#ifndef TELNET_SHELL_STK
#define TELNET_SHELL_STK 16384
#endif

/* Códigos del protocolo telnet */
#define TELNET_SE       240
#define TELNET_NOP      241
#define TELNET_DM       242
#define TELNET_BRK      243
#define TELNET_IP       244
#define TELNET_AO       245
#define TELNET_AYT      246
#define TELNET_EC       247
#define TELNET_EL       248
#define TELNET_GA       249
#define TELNET_SB       250
#define TELNET_WILL     251
#define TELNET_WONT     252
#define TELNET_DO       253
#define TELNET_DONT     254
#define TELNET_IAC      255

/* Opciones que nos interesan */
#define TELNET_OPT_BINARY   0
#define TELNET_OPT_ECHO     1
#define TELNET_OPT_SGA      3

/* Estados del device */
#define TELNET_STATE_FREE   0
#define TELNET_STATE_ALLOC  1
#define TELNET_STATE_OPEN   2

/* Estados del parser IAC */
#define TELNET_IAC_NONE     0
#define TELNET_IAC_CMD      1
#define TELNET_IAC_OPT      2
#define TELNET_IAC_SB       3
#define TELNET_IAC_SB_IAC   4

/**
 * Bloque de control de una conexión telnet.
 */
struct telnet
{
    uchar state;                /**< TELNET_STATE_*                     */
    uint  gen;                  /**< generación de sesión (anti-rezagos) */
    bool  echo;                 /**< eco local activo                   */
    bool  ieof;                 /**< EOF pendiente                      */
    bool  tclosed;              /**< el peer cerró (FIN/error)          */
    bool  dying;                /**< el shell terminó; falta reap       */
    bool  skip_lf;              /**< NVT: CR previo (comer LF/NUL)      */
    uchar iacstate;             /**< parser IAC (TELNET_IAC_*)           */
    uchar iaccmd;               /**< comando IAC en curso               */
    uint  overrun;              /**< bytes descartados por buffer lleno */

    /* Entrada de red -> shell */
    semaphore isem;             /**< bytes disponibles en in[] (persistente) */
    char  in[TELNET_RXBUF];
    uint  istart;
    uint  icount;

    /* Salida shell -> red */
    semaphore osem;             /**< huecos libres en out[] (persistente)    */
    char  out[TELNET_OBLEN];
    uint  ostart;
    uint  ocount;

    struct tcp_pcb *pcb;        /**< conexión (sólo NET task)           */
};

extern struct telnet telnettab[];

/* Driver (nivel device) */
int     telnetAlloc(void);
devcall telnetInit(device *);
devcall telnetOpen(device *, va_list);
devcall telnetClose(device *);
devcall telnetRead(device *, void *, uint);
devcall telnetWrite(device *, const void *, uint);
devcall telnetGetc(device *);
devcall telnetPutc(device *, char);
devcall telnetControl(device *, int, long, long);

/* Lado red (NET task).  telnet_service() y telnet_server_poll_request() se
 * llaman desde el bucle de netTask; el resto lo usan los hilos. */
void telnet_service(void);
void telnet_server_poll_request(void);
void telnet_server_request(bool start);
void telnet_server_set_port(int port);
bool telnet_server_running(void);
int  telnet_server_clients(void);

/* Marca una conexión como terminada (la llama el hilo shell al salir). */
void telnet_conn_done(int devnum);

#endif                          /* _TELNET_H_ */
