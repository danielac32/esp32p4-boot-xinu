/**
 * @file appmgr.c
 *
 * Tabla fija de apps para el launcher.  Sin asignacion dinamica ni bloqueos
 * pesados: el acceso es cooperativo (flags enteros).  Es aditiva: no cambia el
 * comportamiento de nada que no la llame.
 */
#include <kernel.h>
#include <string.h>
#include <thread.h>
#include <appmgr.h>

static struct app_entry table[APPMGR_MAX];

struct app_entry *appmgr_table(void)
{
    return table;
}

int appmgr_count(void)
{
    return APPMGR_MAX;
}

int appmgr_find_path(const char *path)
{
    int i;

    if (NULL == path)
    {
        return SYSERR;
    }
    for (i = 0; i < APPMGR_MAX; i++)
    {
        if (table[i].state != APP_FREE && 0 == strcmp(table[i].path, path))
        {
            return i;
        }
    }
    return SYSERR;
}

int appmgr_find_tid(tid_typ tid)
{
    int i;

    for (i = 0; i < APPMGR_MAX; i++)
    {
        if (table[i].state != APP_FREE && table[i].tid == tid)
        {
            return i;
        }
    }
    return SYSERR;
}

int appmgr_register(const char *name, const char *path, uint32 color, tid_typ tid)
{
    int i, slot = SYSERR;

    /* reutiliza la ranura existente de esa ruta, o busca una libre */
    i = appmgr_find_path(path);
    if (i >= 0)
    {
        slot = i;
    }
    else
    {
        for (i = 0; i < APPMGR_MAX; i++)
        {
            if (APP_FREE == table[i].state)
            {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
    {
        return SYSERR;
    }

    memset(&table[slot], 0, sizeof(table[slot]));
    if (name)
    {
        strncpy(table[slot].name, name, sizeof(table[slot].name) - 1);
    }
    if (path)
    {
        strncpy(table[slot].path, path, sizeof(table[slot].path) - 1);
    }
    table[slot].color = color;
    table[slot].tid = tid;
    table[slot].state = APP_RUNNING;
    table[slot].info = 0;
    return slot;
}

int appmgr_unregister(int slot)
{
    if (slot < 0 || slot >= APPMGR_MAX)
    {
        return SYSERR;
    }
    memset(&table[slot], 0, sizeof(table[slot]));
    table[slot].state = APP_FREE;
    return OK;
}

int appmgr_get_state(int slot)
{
    if (slot < 0 || slot >= APPMGR_MAX)
    {
        return APP_FREE;
    }
    return table[slot].state;
}

void appmgr_set_state(int slot, int state)
{
    if (slot < 0 || slot >= APPMGR_MAX)
    {
        return;
    }
    table[slot].state = state;
}

void appmgr_request_close(int slot)
{
    if (slot >= 0 && slot < APPMGR_MAX)
    {
        table[slot].state = APP_CLOSING;
    }
}

int appmgr_take_foreground(int slot)
{
    int i;

    if (slot < 0 || slot >= APPMGR_MAX)
    {
        return SYSERR;
    }
    for (i = 0; i < APPMGR_MAX; i++)
    {
        if (APP_RUNNING == table[i].state)
        {
            table[i].state = APP_BACKGROUND;
        }
    }
    table[slot].state = APP_RUNNING;
    return OK;
}

void appmgr_set_info(int slot, int info)
{
    if (slot >= 0 && slot < APPMGR_MAX)
    {
        table[slot].info = info;
    }
}

int appmgr_get_info(int slot)
{
    if (slot < 0 || slot >= APPMGR_MAX)
    {
        return 0;
    }
    return table[slot].info;
}
