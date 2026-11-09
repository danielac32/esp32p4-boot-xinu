/**
 * @file xsh_http.h
 *
 * Definiciones compartidas entre el servidor (`shell/xsh_httpd.c`) y la API
 * REST que sirve los ficheros de la microSD (`shell/httpapi.c`).
 *
 * Puerto de la misma cabecera de xinu-s3-amp. Lo que alli eran llamadas al
 * helper del FAT del S3 (full_path(), fat_mount(), fs_remove_file()) aqui se
 * resuelve con el layer equivalente del P4: fs_resolve() y fs_mounted() de
 * `shell/fsutil.c`, y fl_remove() de la libreria FAT.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#ifndef _XSH_HTTP_H_
#define _XSH_HTTP_H_

#include <fsutil.h>             /* FS_PATH_MAX */

/** Metodo, path y query de un request ya parseado.  Los tamanos salen del
 *  maximo de linea que lee el servidor (HTTPD_RQMAX). */
struct http_request
{
    char method[8];
    char path[FS_PATH_MAX];     /**< 128: el mismo techo que fs_resolve() */
    char query[256];
};

/**
 * Atiende un request de /api/...
 *
 * @param dev      device TCP del que se lee el body y en el que se escribe
 * @param req      method, path y query ya parseados
 * @param prebody  bytes leidos despues del doble CRLF de cabeceras
 * @param prelen   bytes validos de @p prebody
 * @param bodylen  Content-Length, o -1 si el header no estaba
 * @return OK si se pudo responder, SYSERR si el write fallo
 */
int httpApiHandle(int dev, const struct http_request *req,
                  const char *prebody, int prelen, long bodylen);

#endif                          /* _XSH_HTTP_H_ */
