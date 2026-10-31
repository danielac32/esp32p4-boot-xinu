
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <fsutil.h>
#include <fat.h>

shellcmd xsh_test(int nargs, char *args[]){
    int sd= control(FAT0, FAT_CTRL_MOUNTED, 0, 0);
    printf("%d\n", sd );
	return OK;
}
