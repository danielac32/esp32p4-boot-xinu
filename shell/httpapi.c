/**
 * @file httpapi.c
 *
 * API REST del httpd: el gestor de ficheros de la microSD.
 *
 *   GET    /api/files?path=/dir      lista un directorio (JSON)
 *   GET    /api/download?path=/file  baja un fichero
 *   GET    /api/shot                 la pantalla actual en BMP
 *   PUT    /api/file?path=/file      sube un fichero (body crudo)
 *   DELETE /api/file?path=/file      borra un fichero
 *   POST   /api/mkdir?path=/dir      crea un directorio (y sus padres)
 *   POST   /api/rmdir?path=/dir[&recursive=1]  elimina un directorio
 *
 * PUT /api/file y POST /api/mkdir crean los directorios que falten.  Las
 * rutas son absolutas dentro de la tarjeta: fs_resolve() colapsa "." y ".."
 * y frena en el raiz, asi que un request no puede salirse de la SD.
 *
 * Puerto de `shell/httpapi.c' de xinu-s3-amp con tres cambios de fondo, no
 * de estilo:
 *  - el FAT del P4 ya esta montado por el device `fat' al arrancar (FAT0 en
 *    el devtab), asi que no existe fat_mount(); se comprueba con
 *    fs_mounted() y se usa fl_* sobre `fsutil.c';
 *  - full_path(), fs_remove_file() y fs_dir_is_empty() del S3 no estan en el
 *    P4: fl_remove() y un empty-check local los cubren;
 *  - caen /api/exec y /api/job, que dependen de webterm, todavia no portado
 *    (tarea W6-S4).
 *
 * Los buffers grandes son static a proposito.  Solo hay un servidor y atiende
 * un request cada vez, y la regla del proyecto es no tirar de pila para los
 * buffers de cada atencion: el bucle del httpd vive con 8 KB de pila y una
 * escritura al FAT mete varios niveles de llamada por debajo.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <conf.h>

#if NTCP

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <device.h>
#include <shell.h>
#include <fat.h>
#include <fat_filelib.h>
#include <lvgl.h>                  /* lvglShotBmp: foto de la pantalla */
#include <fsutil.h>
#include <xsh_http.h>

#define API_PARAM       512
#define API_BUF         1024
#define API_JSON        512
#define API_ESC         256
#define API_NAME        80
#define API_MAX_UPLOAD  (16L * 1024L * 1024L)

/* static: ver la nota del header del fichero. */
static char apiFull[FS_PATH_MAX];
static char apiRaw[API_PARAM];
static char apiEsc[API_ESC];
static char apiJson[API_JSON];
static char apiBuf[API_BUF];

static int write_all(int dev, const void *buf, int len)
{
    const char *p = (const char *)buf;

    while (len > 0)
    {
        int n = write(dev, p, len);

        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static void json_escape(const char *src, char *dst, int dstlen)
{
    static const char hex[] = "0123456789abcdef";
    int o = 0;

    if (dstlen <= 0)
        return;

    while (*src && o < dstlen - 1)
    {
        unsigned char c = (unsigned char)*src++;

        if (c == '"' || c == '\\')
        {
            if (o + 2 >= dstlen)
                break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        }
        else if (c < 0x20)
        {
            if (o + 6 >= dstlen)
                break;
            dst[o++] = '\\';
            dst[o++] = 'u';
            dst[o++] = '0';
            dst[o++] = '0';
            dst[o++] = hex[(c >> 4) & 0x0f];
            dst[o++] = hex[c & 0x0f];
        }
        else
        {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static int send_headers(int dev, int code, const char *reason,
                        const char *ctype, long len, const char *disp)
{
    char hdr[320];
    int n;

    n = sprintf(hdr, "HTTP/1.0 %d %s\r\n", code, reason);
    n += sprintf(hdr + n, "Content-Type: %s\r\n", ctype);
    if (len >= 0)
        n += sprintf(hdr + n, "Content-Length: %ld\r\n", len);
    if (disp && disp[0])
        n += sprintf(hdr + n,
                     "Content-Disposition: attachment; filename=\"%s\"\r\n",
                     disp);
    n += sprintf(hdr + n, "Connection: close\r\n\r\n");

    return write_all(dev, hdr, n);
}

static int send_json(int dev, int code, const char *reason, const char *json)
{
    if (send_headers(dev, code, reason, "application/json",
                     (long)strlen(json), NULL) < 0)
        return SYSERR;
    return write_all(dev, json, strlen(json));
}

static int send_json_error(int dev, int code, const char *reason,
                           const char *msg)
{
    json_escape(msg, apiEsc, sizeof(apiEsc));
    sprintf(apiJson, "{\"ok\":false,\"error\":\"%s\"}", apiEsc);
    return send_json(dev, code, reason, apiJson);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decodifica %XX y '+' en el valor de una query.  Rechaza bytes NUL. */
static int url_decode(const char *src, char *dst, int dstlen)
{
    int o = 0;

    while (*src)
    {
        unsigned char c = (unsigned char)*src++;

        if (c == '+')
        {
            c = ' ';
        }
        else if (c == '%')
        {
            int hi;
            int lo;

            if (!src[0] || !src[1])
                return -1;
            hi = hexval((unsigned char)src[0]);
            lo = hexval((unsigned char)src[1]);

            if (hi < 0 || lo < 0)
                return -1;
            src += 2;
            c = (unsigned char)((hi << 4) | lo);
            if (c == 0)
                return -1;
        }

        if (o >= dstlen - 1)
            return -1;
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return 0;
}

/* Busca "name=value" en la query.  1 si esta, 0 si no, -1 si esta mal
 * formado o no cabe. */
static int get_param(const char *query, const char *name,
                     char *out, int outlen)
{
    const char *p = query;
    int nlen = (int)strlen(name);

    while (p && *p)
    {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = strchr(p, '=');

        if (eq && eq < end && (int)(eq - p) == nlen
            && strncmp(p, name, nlen) == 0)
        {
            int vlen = (int)(end - (eq + 1));
            char tmp[API_PARAM];

            if (vlen < 0 || vlen >= (int)sizeof(tmp))
                return -1;
            memcpy(tmp, eq + 1, vlen);
            tmp[vlen] = '\0';
            return url_decode(tmp, out, outlen) == 0 ? 1 : -1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* path=... de la query convertido a ruta absoluta normalizada de la SD. */
static int resolve_path(const struct http_request *req, char *full, int fullsz)
{
    int r;

    r = get_param(req->query, "path", apiRaw, sizeof(apiRaw));
    if (r < 0)
        return -1;
    if (r == 0)
        strcpy(apiRaw, "/");
    if (apiRaw[0] != '/')
        return -1;

    if (!fs_resolve(apiRaw, full, (size_t)fullsz))
        return -1;
    return 0;
}

/* Crea todos los directorios que falten por encima de @p full.  La ruta ya
 * esta resuelta, asi que ".." no puede sacar la operacion de la tarjeta. */
static int ensure_parent_dirs(const char *full)
{
    char tmp[FS_PATH_MAX];
    char *slash;
    char *p;

    if (!full || full[0] != '/' || strlen(full) >= sizeof(tmp))
        return -1;

    strcpy(tmp, full);
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';

    for (p = tmp + 1; ; p++)
    {
        if (*p == '/' || *p == '\0')
        {
            char saved = *p;

            *p = '\0';
            if (tmp[0] != '\0' && !fl_is_dir(tmp))
            {
                if (fl_createdirectory(tmp) != 1 && !fl_is_dir(tmp))
                    return -1;
            }
            *p = saved;
            if (saved == '\0')
                break;
        }
    }

    return 0;
}

static void base_name(const char *path, char *out, int outlen)
{
    const char *p = strrchr(path, '/');
    int o = 0;

    p = p ? p + 1 : path;
    while (*p && o < outlen - 1)
    {
        char c = *p++;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[o++] = c;
        else
            out[o++] = '_';
    }
    if (o == 0)
    {
        strcpy(out, "download.bin");
        return;
    }
    out[o] = '\0';
}

/* El S3 tenia fs_dir_is_empty(); aqui se resuelve con la propia libreria. */
static int dir_is_empty(const char *path)
{
    FL_DIR dir;
    fl_dirent ent;

    if (fl_opendir(path, &dir) == NULL)
        return -1;
    while (fl_readdir(&dir, &ent) == 0)
    {
        if (strcmp(ent.filename, ".") && strcmp(ent.filename, ".."))
        {
            fl_closedir(&dir);
            return 0;
        }
    }
    fl_closedir(&dir);
    return 1;
}

static int api_files(int dev, const struct http_request *req)
{
    FL_DIR dir;
    fl_dirent ent;
    char item[384];
    int first = 1;

    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");
    if (resolve_path(req, apiFull, sizeof(apiFull)) < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid path");
    if (fl_opendir(apiFull, &dir) == NULL)
        return send_json_error(dev, 404, "Not Found", "cannot open directory");

    if (send_headers(dev, 200, "OK", "application/json", -1, NULL) < 0)
    {
        fl_closedir(&dir);
        return SYSERR;
    }

    json_escape(apiFull, apiEsc, sizeof(apiEsc));
    sprintf(item, "{\"path\":\"%s\",\"entries\":[", apiEsc);
    if (write_all(dev, item, strlen(item)) < 0)
    {
        fl_closedir(&dir);
        return SYSERR;
    }

    while (fl_readdir(&dir, &ent) == 0)
    {
        if (!strcmp(ent.filename, ".") || !strcmp(ent.filename, ".."))
            continue;

        json_escape(ent.filename, apiEsc, sizeof(apiEsc));
        sprintf(item, "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lu}",
                first ? "" : ",", apiEsc,
                ent.is_dir ? "true" : "false",
                (unsigned long)ent.size);
        if (write_all(dev, item, strlen(item)) < 0)
        {
            fl_closedir(&dir);
            return SYSERR;
        }
        first = 0;
    }

    fl_closedir(&dir);
    return write_all(dev, "]}", 2);
}

static int api_download(int dev, const struct http_request *req)
{
    char name[API_NAME];
    void *f;
    long len;
    int n;

    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");
    if (resolve_path(req, apiFull, sizeof(apiFull)) < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid path");
    if (fl_is_dir(apiFull))
        return send_json_error(dev, 400, "Bad Request", "is a directory");

    f = fl_fopen(apiFull, "r");
    if (!f)
        return send_json_error(dev, 404, "Not Found", "cannot open file");

    fl_fseek(f, 0, 2);
    len = fl_ftell(f);
    fl_fseek(f, 0, 0);
    base_name(apiFull, name, sizeof(name));

    if (send_headers(dev, 200, "OK", "application/octet-stream", len, name) < 0)
    {
        fl_fclose(f);
        return SYSERR;
    }

    while ((n = fl_fread(apiBuf, 1, sizeof(apiBuf), f)) > 0)
    {
        if (write_all(dev, apiBuf, n) < 0)
            break;
    }

    fl_fclose(f);
    return OK;
}

static int api_upload(int dev, const struct http_request *req,
                      const char *prebody, int prelen, long bodylen)
{
    void *f;
    long written = 0;
    long remaining;
    int n;

    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");
    if (resolve_path(req, apiFull, sizeof(apiFull)) < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid path");
    if (fl_is_dir(apiFull))
        return send_json_error(dev, 409, "Conflict", "path is a directory");
    if (bodylen < 0)
        return send_json_error(dev, 411, "Length Required",
                               "Content-Length is required");
    if (bodylen > API_MAX_UPLOAD)
        return send_json_error(dev, 413, "Payload Too Large",
                               "file exceeds the 16 MB limit");
    if (ensure_parent_dirs(apiFull) < 0)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot create parent directory");

    /* La libreria FAT del P4 no trunca: fl_fopen(..., "w") sobre un nombre que
     * ya existe abre el existente con su talla vieja (FILE_ERASE se marca en
     * fat_filelib.c:735 y nadie lo lee; la rama "Write Existing" de las lineas
     * 791-795 devuelve el fichero con el filelength y los clusters viejos).
     * Sin este borrado, un PUT mas
     * pequeno que el fichero anterior dejaria la cola de los datos viejos y el
     * filelength viejo.  Mide con `ls -l` y se ve: 76.834 B -> 100 B daria
     * 76.834. */
    if (fs_exists(apiFull))
    {
        fl_remove(apiFull);
    }

    f = fl_fopen(apiFull, "w");
    if (!f)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot create file");

    if (prelen < 0)
        prelen = 0;
    if ((long)prelen > bodylen)
        prelen = (int)bodylen;

    if (prelen > 0)
    {
        n = fl_fwrite(prebody, 1, prelen, f);
        if (n != prelen)
        {
            fl_fclose(f);
            fl_remove(apiFull);
            return send_json_error(dev, 500, "Internal Server Error",
                                   "cannot write file");
        }
        written += prelen;
    }

    remaining = bodylen - written;
    while (remaining > 0)
    {
        int want = remaining > (long)sizeof(apiBuf) ? (int)sizeof(apiBuf)
                                                    : (int)remaining;

        n = read(dev, apiBuf, want);
        if (n <= 0)
            break;
        if (fl_fwrite(apiBuf, 1, n, f) != n)
        {
            fl_fclose(f);
            fl_remove(apiFull);
            return send_json_error(dev, 500, "Internal Server Error",
                                   "cannot write file");
        }
        written += n;
        remaining -= n;
    }

    fl_fclose(f);

    if (written != bodylen)
    {
        fl_remove(apiFull);
        return send_json_error(dev, 400, "Bad Request", "incomplete body");
    }

    json_escape(apiFull, apiEsc, sizeof(apiEsc));
    sprintf(apiJson, "{\"ok\":true,\"path\":\"%s\",\"size\":%ld}",
            apiEsc, written);
    return send_json(dev, 201, "Created", apiJson);
}

static int api_delete(int dev, const struct http_request *req)
{
    void *probe;

    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");
    if (resolve_path(req, apiFull, sizeof(apiFull)) < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid path");
    if (fl_is_dir(apiFull))
        return send_json_error(dev, 409, "Conflict",
                               "path is a directory; use rmdir");

    probe = fl_fopen(apiFull, "r");
    if (!probe)
        return send_json_error(dev, 404, "Not Found", "file not found");
    fl_fclose(probe);

    if (fl_remove(apiFull) != 0)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot delete file");

    return send_json(dev, 200, "OK", "{\"ok\":true}");
}

static int api_mkdir(int dev, const struct http_request *req)
{
    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");
    if (resolve_path(req, apiFull, sizeof(apiFull)) < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid path");
    if (fl_is_dir(apiFull))
        return send_json_error(dev, 409, "Conflict", "directory exists");
    if (ensure_parent_dirs(apiFull) < 0)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot create parent directory");
    if (fl_createdirectory(apiFull) != 1)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot create directory");

    return send_json(dev, 201, "Created", "{\"ok\":true}");
}

static int api_rmdir(int dev, const struct http_request *req)
{
    char rec[8];
    int recursive = 0;
    int r;

    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");
    if (resolve_path(req, apiFull, sizeof(apiFull)) < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid path");
    if (!strcmp(apiFull, "/"))
        return send_json_error(dev, 400, "Bad Request",
                               "cannot remove the root directory");
    if (!fl_is_dir(apiFull))
        return send_json_error(dev, 400, "Bad Request", "not a directory");

    r = get_param(req->query, "recursive", rec, sizeof(rec));
    if (r < 0)
        return send_json_error(dev, 400, "Bad Request", "invalid recursive");
    if (r == 1 && (!strcmp(rec, "1") || !strcmp(rec, "true")))
        recursive = 1;

    if (recursive)
    {
        if (fs_remove_tree(apiFull) != OK)
            return send_json_error(dev, 500, "Internal Server Error",
                                   "cannot remove directory tree");
    }
    else
    {
        int empty = dir_is_empty(apiFull);

        if (empty < 0)
            return send_json_error(dev, 500, "Internal Server Error",
                                   "cannot inspect directory");
        if (!empty)
            return send_json_error(dev, 409, "Conflict",
                                   "directory is not empty");
        if (fl_removedirectory(apiFull) != 0)
            return send_json_error(dev, 500, "Internal Server Error",
                                   "cannot remove directory");
    }

    return send_json(dev, 200, "OK", "{\"ok\":true}");
}

/**
 * GET /api/shot -- la pantalla, tal y como la tiene LVGL ahora mismo.
 *
 * El panel es de solo escritura y una app en primer plano deja la consola
 * ocupada (elfrun corre en la hebra de la shell), asi que sin esto no hay
 * forma de mirar la interfaz desde el PC.  Vuelca lvgl_shadow a /shot.bmp y
 * sirve el BMP; es el mismo fichero que deja 'lcd shot'.
 */
static int api_shot(int dev)
{
    void *f;
    long  len;
    int   n;
    int   rc;

    if (!fs_mounted())
        return send_json_error(dev, 503, "Service Unavailable",
                               "FAT not mounted");

    /* El espejo lo escribe la hebra lvglui en cada flush: el candado evita
     * fotografiar una franja a medio copiar. */
    lvgl_lock();
    rc = lvglShotBmp("/shot.bmp");
    lvgl_unlock();
    if (SYSERR == rc)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot write shot");

    f = fl_fopen("/shot.bmp", "r");
    if (NULL == f)
        return send_json_error(dev, 500, "Internal Server Error",
                               "cannot open shot");
    fl_fseek(f, 0, 2);
    len = fl_ftell(f);
    fl_fseek(f, 0, 0);
    if (send_headers(dev, 200, "OK", "image/bmp", len, "shot.bmp") < 0)
    {
        fl_fclose(f);
        return SYSERR;
    }
    while ((n = fl_fread(apiBuf, 1, sizeof(apiBuf), f)) > 0)
    {
        if (write_all(dev, apiBuf, n) < 0)
            break;
    }
    fl_fclose(f);
    return OK;
}

int httpApiHandle(int dev, const struct http_request *req,
                  const char *prebody, int prelen, long bodylen)
{
    if (!strcmp(req->path, "/api/files"))
    {
        if (!strcmp(req->method, "GET"))
            return api_files(dev, req);
        return send_json_error(dev, 405, "Method Not Allowed", "use GET");
    }

    if (!strcmp(req->path, "/api/download"))
    {
        if (!strcmp(req->method, "GET"))
            return api_download(dev, req);
        return send_json_error(dev, 405, "Method Not Allowed", "use GET");
    }

    if (!strcmp(req->path, "/api/shot"))
    {
        if (!strcmp(req->method, "GET"))
            return api_shot(dev);
        return send_json_error(dev, 405, "Method Not Allowed", "use GET");
    }

    if (!strcmp(req->path, "/api/file"))
    {
        if (!strcmp(req->method, "PUT"))
            return api_upload(dev, req, prebody, prelen, bodylen);
        if (!strcmp(req->method, "DELETE"))
            return api_delete(dev, req);
        return send_json_error(dev, 405, "Method Not Allowed",
                               "use PUT or DELETE");
    }

    if (!strcmp(req->path, "/api/mkdir"))
    {
        if (!strcmp(req->method, "POST"))
            return api_mkdir(dev, req);
        return send_json_error(dev, 405, "Method Not Allowed", "use POST");
    }

    if (!strcmp(req->path, "/api/rmdir"))
    {
        if (!strcmp(req->method, "POST"))
            return api_rmdir(dev, req);
        return send_json_error(dev, 405, "Method Not Allowed", "use POST");
    }

    return send_json_error(dev, 404, "Not Found", "unknown endpoint");
}

#endif                          /* NTCP */
