/*
 * Simple Java Virtual Machine Implementation
 *
 * Copyright (C) 2013 Chun-Yu Wang <wicanr2@gmail.com>
 */

#include "java_lib.h"
#include "jvm_map.h"

/* ---- Natives de plataforma (un fichero C por modulo) --------------
 * Host: jvm/esp32p4_{hw,io,fat,sys}.c · P4: device/jvm/native_{hw,io,fat,sys}.c
 * Math.random vive en la plataforma (necesita el RNG/reloj). */
extern int java_lang_math_random(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.HW — hardware (tiempo, gpio, sonido) */
extern int esp32p4_hw_delay(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_millis(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_gpioWrite(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_gpioRead(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_sound(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_memTotal(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_memFree(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_memMaxBlock(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_psramTotal(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_psramFree(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_psramMaxBlock(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_version(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_hw_clkFreq(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_rgb_set(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_rgb_off(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_rgb_get(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.IO — consola y texto */
extern int esp32p4_io_print(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_io_println(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_io_readChar(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_io_strLen(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_io_strAt(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_io_printBuf(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.FAT — ficheros/directorios (estilo java.io) */
extern int esp32p4_fat_open(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_close(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_read(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_write(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_seek(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_tell(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_eof(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_readLine(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_writeString(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_readAll(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_writeAll(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_appendString(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_exists(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_isDirectory(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_length(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_delete(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_mkdir(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_rmdir(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_fat_list(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.Sys — servicios del kernel */
extern int esp32p4_sys_run(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_runHere(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_kill(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_start(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_send(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_recv(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_alive(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_parent(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_stackPeak(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_stackTotal(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.LVGL — UI (LVGL v8) */
extern int esp32p4_lvgl_init(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_screen(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_clear(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_label(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_button(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_rect(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setText(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setTextFmt(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setPos(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setSize(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setColor(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setRadius(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setFlat(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setPad(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_setFont(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_tick(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_idleOff(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_pollEvent(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_eventWidget(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_eventCode(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_eventGesture(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_activeScreen(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_loadScreen(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_del(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_img(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_getWidth(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_getHeight(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_screenWidth(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_lvgl_screenHeight(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.PMIC no existe en este puerto: el ESP32-S3 no lleva PMIC. */

/* esp32p4.Power — PMIC AXP2101 de solo lectura (device/jvm/native_hw.c) */
extern int esp32p4_power_percent(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_power_voltage(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_power_charge(StackFrame *, SimpleConstantPool *, char *);

/* esp32p4.Net — red (minimo: IP y estado) */
extern int esp32p4_net_ip(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_net_isConnected(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_sleep(StackFrame *, SimpleConstantPool *, char *);
/* Thread.sleep(long) -> esp32p4.Sys.sleep(int) */
extern int esp32p4_sys_yield(StackFrame *, SimpleConstantPool *, char *);
extern int esp32p4_sys_tid(StackFrame *, SimpleConstantPool *, char *);

/* java_std.c — natives puras (host + target) */
extern int java_lang_integer_parseInt(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_integer_toString(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_valueOf(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_boolean_toString(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_float_toString(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_character_toString(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_math_abs(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_math_max(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_math_min(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_substring(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_indexOf(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_toUpper(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_toLower(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_trim(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_startsWith(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_endsWith(StackFrame *, SimpleConstantPool *, char *);
extern int java_lang_string_concat(StackFrame *, SimpleConstantPool *, char *);

static java_lang_method method_table[] = {
    /* ---- java.lang: Math y conversiones (java_std.c) ---- */
    {"java/lang/Math", "random", java_lang_math_random},
    {"java/lang/Math", "abs",    java_lang_math_abs},
    {"java/lang/Math", "max",    java_lang_math_max},
    {"java/lang/Math", "min",    java_lang_math_min},
    {"java/lang/Integer", "parseInt", java_lang_integer_parseInt},
    {"java/lang/Integer", "toString", java_lang_integer_toString},
    {"java/lang/String", "valueOf",   java_lang_string_valueOf},
    {"java/lang/Boolean", "toString", java_lang_boolean_toString},
    {"java/lang/Float",   "toString", java_lang_float_toString},
    {"java/lang/Character", "toString", java_lang_character_toString},
    /* java.lang.String: metodos de instancia (via op_invokevirtual) */
    {"java/lang/String", "substring",   java_lang_string_substring},
    {"java/lang/String", "indexOf",     java_lang_string_indexOf},
    {"java/lang/String", "toUpperCase", java_lang_string_toUpper},
    {"java/lang/String", "toLowerCase", java_lang_string_toLower},
    {"java/lang/String", "trim",        java_lang_string_trim},
    {"java/lang/String", "startsWith",  java_lang_string_startsWith},
    {"java/lang/String", "endsWith",    java_lang_string_endsWith},
    {"java/lang/String", "concat",      java_lang_string_concat},

    /* ---- esp32p4.HW: hardware ---- */
    {"esp32p4/HW", "delay",     esp32p4_hw_delay},
    {"esp32p4/HW", "millis",    esp32p4_hw_millis},
    {"esp32p4/HW", "gpioWrite", esp32p4_hw_gpioWrite},
    {"esp32p4/HW", "gpioRead",  esp32p4_hw_gpioRead},
    {"esp32p4/HW", "sound",     esp32p4_hw_sound},
    {"esp32p4/HW", "memTotal",    esp32p4_hw_memTotal},
    {"esp32p4/HW", "memFree",     esp32p4_hw_memFree},
    {"esp32p4/HW", "memMaxBlock", esp32p4_hw_memMaxBlock},
    {"esp32p4/HW", "psramTotal",    esp32p4_hw_psramTotal},
    {"esp32p4/HW", "psramFree",     esp32p4_hw_psramFree},
    {"esp32p4/HW", "psramMaxBlock", esp32p4_hw_psramMaxBlock},
    {"esp32p4/HW", "version",  esp32p4_hw_version},
    {"esp32p4/HW", "clkFreq",  esp32p4_hw_clkFreq},
    {"esp32p4/RGB", "set",      esp32p4_rgb_set},
    {"esp32p4/RGB", "off",      esp32p4_rgb_off},
    {"esp32p4/RGB", "get",      esp32p4_rgb_get},

    /* ---- esp32p4.Power: bateria (solo lectura) ---- */
    {"esp32p4/Power", "percent",  esp32p4_power_percent},
    {"esp32p4/Power", "voltage",  esp32p4_power_voltage},
    {"esp32p4/Power", "charge",   esp32p4_power_charge},

    /* ---- esp32p4.IO: consola y texto ---- */
    {"esp32p4/IO", "print",    esp32p4_io_print},
    {"esp32p4/IO", "println",  esp32p4_io_println},
    {"esp32p4/IO", "readChar", esp32p4_io_readChar},
    {"esp32p4/IO", "strLen",   esp32p4_io_strLen},
    {"esp32p4/IO", "strAt",    esp32p4_io_strAt},
    {"esp32p4/IO", "printBuf", esp32p4_io_printBuf},

    /* ---- esp32p4.FAT: ficheros/directorios (estilo java.io) ---- */
    {"esp32p4/FAT", "open",         esp32p4_fat_open},
    {"esp32p4/FAT", "close",        esp32p4_fat_close},
    {"esp32p4/FAT", "read",         esp32p4_fat_read},
    {"esp32p4/FAT", "write",        esp32p4_fat_write},
    {"esp32p4/FAT", "seek",         esp32p4_fat_seek},
    {"esp32p4/FAT", "tell",         esp32p4_fat_tell},
    {"esp32p4/FAT", "eof",          esp32p4_fat_eof},
    {"esp32p4/FAT", "readLine",     esp32p4_fat_readLine},
    {"esp32p4/FAT", "writeString",  esp32p4_fat_writeString},
    {"esp32p4/FAT", "readAll",      esp32p4_fat_readAll},
    {"esp32p4/FAT", "writeAll",     esp32p4_fat_writeAll},
    {"esp32p4/FAT", "appendString", esp32p4_fat_appendString},
    {"esp32p4/FAT", "exists",       esp32p4_fat_exists},
    {"esp32p4/FAT", "isDirectory",  esp32p4_fat_isDirectory},
    {"esp32p4/FAT", "length",       esp32p4_fat_length},
    {"esp32p4/FAT", "delete",       esp32p4_fat_delete},
    {"esp32p4/FAT", "mkdir",        esp32p4_fat_mkdir},
    {"esp32p4/FAT", "rmdir",        esp32p4_fat_rmdir},
    {"esp32p4/FAT", "list",         esp32p4_fat_list},

    /* ---- esp32p4.Sys: kernel ---- */
    {"esp32p4/Sys", "run",     esp32p4_sys_run},
    {"esp32p4/Sys", "runHere", esp32p4_sys_runHere},
    {"esp32p4/Sys", "kill",  esp32p4_sys_kill},
    {"esp32p4/Sys", "sleep", esp32p4_sys_sleep},
    {"java/lang/Thread", "sleep", esp32p4_sys_sleep},
    {"esp32p4/Sys", "yield", esp32p4_sys_yield},
    {"esp32p4/Sys", "tid",   esp32p4_sys_tid},
    {"esp32p4/Sys", "start", esp32p4_sys_start},
    {"esp32p4/Sys", "send",  esp32p4_sys_send},
    {"esp32p4/Sys", "recv",  esp32p4_sys_recv},
    {"esp32p4/Sys", "alive", esp32p4_sys_alive},
    {"esp32p4/Sys", "parent",     esp32p4_sys_parent},
    {"esp32p4/Sys", "stackPeak",  esp32p4_sys_stackPeak},
    {"esp32p4/Sys", "stackTotal", esp32p4_sys_stackTotal},

    /* ---- esp32p4.LVGL: UI ---- */
    {"esp32p4/LVGL", "init",         esp32p4_lvgl_init},
    {"esp32p4/LVGL", "screen",       esp32p4_lvgl_screen},
    {"esp32p4/LVGL", "clear",        esp32p4_lvgl_clear},
    {"esp32p4/LVGL", "label",        esp32p4_lvgl_label},
    {"esp32p4/LVGL", "button",       esp32p4_lvgl_button},
    {"esp32p4/LVGL", "rect",         esp32p4_lvgl_rect},
    {"esp32p4/LVGL", "setText",      esp32p4_lvgl_setText},
    {"esp32p4/LVGL", "setTextFmt",   esp32p4_lvgl_setTextFmt},
    {"esp32p4/LVGL", "setPos",       esp32p4_lvgl_setPos},
    {"esp32p4/LVGL", "setSize",      esp32p4_lvgl_setSize},
    {"esp32p4/LVGL", "setColor",     esp32p4_lvgl_setColor},
    {"esp32p4/LVGL", "setRadius",    esp32p4_lvgl_setRadius},
    {"esp32p4/LVGL", "setFlat",      esp32p4_lvgl_setFlat},
    {"esp32p4/LVGL", "setPad",       esp32p4_lvgl_setPad},
    {"esp32p4/LVGL", "setFont",      esp32p4_lvgl_setFont},
    {"esp32p4/LVGL", "tick",         esp32p4_lvgl_tick},
    {"esp32p4/LVGL", "idleOff",      esp32p4_lvgl_idleOff},
    {"esp32p4/LVGL", "pollEvent",    esp32p4_lvgl_pollEvent},
    {"esp32p4/LVGL", "eventWidget",  esp32p4_lvgl_eventWidget},
    {"esp32p4/LVGL", "eventCode",    esp32p4_lvgl_eventCode},
    {"esp32p4/LVGL", "eventGesture", esp32p4_lvgl_eventGesture},
    {"esp32p4/LVGL", "activeScreen", esp32p4_lvgl_activeScreen},
    {"esp32p4/LVGL", "loadScreen",   esp32p4_lvgl_loadScreen},
    {"esp32p4/LVGL", "del",          esp32p4_lvgl_del},
    {"esp32p4/LVGL", "img",          esp32p4_lvgl_img},
    {"esp32p4/LVGL", "getWidth",     esp32p4_lvgl_getWidth},
    {"esp32p4/LVGL", "getHeight",    esp32p4_lvgl_getHeight},
    {"esp32p4/LVGL", "screenWidth",  esp32p4_lvgl_screenWidth},
    {"esp32p4/LVGL", "screenHeight", esp32p4_lvgl_screenHeight},

    /* ---- esp32p4.PMIC: no hay PMIC en el ESP32-S3 ---- */

    /* ---- esp32p4.Net: red (minimo) ---- */
    {"esp32p4/Net", "ip",          esp32p4_net_ip},
    {"esp32p4/Net", "isConnected", esp32p4_net_isConnected},
};

static int java_lang_method_size = sizeof(method_table) / sizeof(java_lang_method);

java_lang_method *find_java_lang_method(char *cls_name, char *method_name)
{
    int i;
    for (i = 0; i < java_lang_method_size; i++)
        if (strcmp(cls_name, method_table[i].clzname) == 0 &&
            strcmp(method_name, method_table[i].methodname) == 0)
            return &method_table[i];
    return 0;
}

int invoke_java_lang_library(StackFrame *stack, SimpleConstantPool *p,
                             char *cls_name, char *method_name, char *type)
{
    java_lang_method *method = find_java_lang_method(cls_name, method_name);
    if (method != 0) {
#if SIMPLE_JVM_DEBUG
        printf("invoke %s/%s %s\n", method->clzname, method->methodname, type);
#endif
        method->method_runtime(stack, p, type);
        return 1;
    }
    return 0;
}
