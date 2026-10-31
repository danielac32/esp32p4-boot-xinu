/*
 * fat_rmdir — fl_removedirectory(), que la lib traía como `#define rmdir(a) 0`.
 *
 * Borra un directorio VACÍO usando sólo API pública de la lib + el gancho
 * fl_get_fs() (FATFS_INC_TEST_HOOKS, -D en el Makefile):
 *  1. debe existir y ser directorio (fl_opendir),
 *  2. debe estar vacío (sólo "." y ".."),
 *  3. se libera su cluster y se marca la entrada como borrada
 *     (igual que fl_remove pero sin su filtro de "sólo ficheros").
 * La raíz se rechaza siempre. Esta función no toma el candado global: usa
 * la API pública (fl_opendir/fl_readdir/...), que ya bloquea donde toca
 * (el candado lo instala el device `fat` vía fl_attach_locks). */

#include "fat_filelib.h"
#include "fat_access.h"
#include "fat_table.h"
#include "fat_misc.h"
#include "fat_string.h"
#include <string.h>

static int is_dot_entry(const char *name)
{
    if (!name || name[0] != '.')
        return 0;
    return name[1] == 0 || (name[1] == '.' && name[2] == 0);
}

int fl_removedirectory(const char *path)
{
    FL_DIR dir, pdir;
    fl_dirent entry;
    char parent[FATFS_MAX_LONG_FILENAME];
    char name[FATFS_MAX_LONG_FILENAME];
    char shortname[FAT_SFN_SIZE_FULL + 1];
    struct fatfs *fs;
    struct fat_dir_entry sfEntry;
    uint32_t parentcluster, start;
    char clean[FATFS_MAX_LONG_FILENAME + 1];
    int n, i;

    if (!path || !*path)
        return -1;

    /* Copia local sin '/' o '\' finales ("dir/" vale, "/" es raíz). */
    n = 0;
    while (path[n] && n < FATFS_MAX_LONG_FILENAME)
        n++;
    if (path[n] != 0)
        return -1;
    memcpy(clean, path, n);
    clean[n] = 0;
    while (n > 0 && (clean[n - 1] == '/' || clean[n - 1] == '\\'))
        clean[--n] = 0;
    if (n == 0)
        return -1;
    path = clean;

    /* 1. Existe y es directorio (además dispara CHECK_FL_INIT). */
    if (!fl_opendir(path, &dir))
        return -1;

    /* 2. Vacío: nada que no sea "." o "..". */
    for (;;) {
        if (fl_readdir(&dir, &entry) != 0)
            break;
        if (!is_dot_entry(entry.filename)) {
            fl_closedir(&dir);
            return -1;
        }
    }
    fl_closedir(&dir);

    /* 3. Separar padre + nombre; sin nombre es la raíz: prohibido. */
    memset(parent, 0, sizeof(parent));
    memset(name, 0, sizeof(name));
    if (fatfs_split_path((char *)path, parent, sizeof(parent),
                         name, sizeof(name)) == -1)
        return -1;
    if (name[0] == 0)
        return -1;

    fs = fl_get_fs();
    if (!fs)
        return -1;

    /* 4. Cluster del padre (raíz o subdirectorio). */
    if (parent[0] == 0) {
        parentcluster = fatfs_get_root_cluster(fs);
    } else {
        if (!fl_opendir(parent, &pdir))
            return -1;
        parentcluster = pdir.cluster;
        fl_closedir(&pdir);
    }

    /* 5. La entrada debe existir y ser directorio. */
    memset(&sfEntry, 0, sizeof(sfEntry));
    if (!fatfs_get_file_entry(fs, parentcluster, name, &sfEntry))
        return -1;
    if (!fatfs_entry_is_dir(&sfEntry))
        return -1;

    /* 6. Liberar su cluster (si tiene) y marcar borrada. */
    start = ((uint32_t)FAT_HTONS((uint32_t)sfEntry.FstClusHI) << 16) |
            (uint32_t)FAT_HTONS((uint32_t)sfEntry.FstClusLO);
    if (start != 0 && !fatfs_free_cluster_chain(fs, start))
        return -1;
    for (i = 0; i < FAT_SFN_SIZE_FULL; i++)
        shortname[i] = (char)sfEntry.Name[i];
    shortname[FAT_SFN_SIZE_FULL] = 0;
    if (!fatfs_mark_file_deleted(fs, parentcluster, shortname))
        return -1;
    return 0;
}
