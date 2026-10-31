# LCD como pantalla de arranque — notas y pendientes

Decisión de diseño (2026-10-31):

- El panel ST7796 **no** es una consola. El proyecto no está pensado para
  tener consola en pantalla.
- La consola del sistema (shell, log, comandos) se queda **solo por UART**.
- El panel muestra **el arranque** una vez; en el futuro se lanzará una
  interfaz gráfica encima.
- El espejo en vivo (`lcd console on`) queda como herramienta opcional de
  depuración, apagado por defecto.

## Corregido en esta ronda

1. **Conflicto dibujo ↔ espejo de consola.** El espejo se activaba en
   `lcdInit()` y cualquier comando de dibujo (`boot`, `demo`, `fill`, `rect`,
   `text`) quedaba pisado por el prompt/eco y borrado en el primer scroll.
   Ahora:
   - el espejo arranca **apagado**;
   - `LCD_CTRL_FILL/RECT/PIXEL/TEXT/BLIT/DEMO` apagan el espejo antes de
     dibujar, así que lo dibujado permanece;
   - el arranque se pinta **una vez** desde el shell (`LCD_CTRL_SYNC`).
2. **`lcd demo` documentado pero inexistente.** Añadida la rama; `lcd` sin
   argumentos y `lcd demo` hacen lo mismo.
3. **Validación de `lcd console`** (`on|off` estricto) y **`lcd rot`**: si el
   espejo está encendido, se repinta la rejilla en la nueva orientación.
4. **Cosmético:** help con el `[` sin cerrar, `devtab[i].name == NULL`
   protegido, textos del help actualizados.
5. **ANSI en el panel:** `lcdConsolePutc()` ahora descarta las secuencias
   `ESC [ ... letra`, que antes se escribían como texto (`[1;31m`) porque el
   panel no interpreta VT100.

## Pendientes / a revisar más adelante

- **`.ld` — sección `.vectors` muerta.** La tabla CLIC vive en flash
  (`.rodata.vtable`, `system/platforms/esp32p4/clic.c`), pero `ld.script`
  conserva una sección `.vectors` (NOLOAD, L2MEM) que ya no usa nadie. No
  rompe; el comentario del linker script es engañoso. No se toca por ahora
  (zona kernel).
- **Coste del scroll con IRQ deshabilitadas.** `kputc()` → `lcdConsolePutc()`
  dibuja ~4 transacciones SPI por carácter. Si un scroll ocurre dentro de un
  `kprintf()` con `disable()`, se pueden perder bytes RX del UART y meter
  jitter al timer. Hoy el espejo está apagado por defecto, así que solo afecta
  a `lcd console on`.
- **Reentrancia del espejo.** `lcdConsole.c` no usa lock a propósito (kprintf
  puede correr con IRQ apagadas). Dos `kprintf` simultáneos pueden mezclar
  celdas. Aceptable mientras el espejo esté apagado.
- **`lcdInit()` ignora el retorno de `moncreate()`.** Hoy hay 4 monitores de
  8; si se agotan, `lock()` recibiría un monitor inválido.
- **`lcd help`** no existe; solo `lcd --help` (`?` lo resuelve el shell).
- **Lista de dispositivos duplicada.** `drawBootScreen()` imprime
  `devtab[i].name` y `naminit()` ya imprime la misma lista al montar.
- **`_start`** no lo provee el linker: está en `loader/platforms/esp32p4/start.S`
  (comentario desactualizado en `xsh_lcd.c`).

## Cómo probar

- Arranque: el panel debe mostrar el log de arranque (últimas 30 líneas) y
  quedarse congelado ahí; la shell sigue por UART.
- `lcd boot`  → pantalla de arranque formateada (versión, banner, memoria,
  dispositivos); se queda.
- `lcd demo`  → barras + texto de bring-up; se queda.
- `lcd console on`  → espejo en vivo otra vez; `lcd fill black` o
  `lcd console off` lo apagan.
- `lcd rot 4` con el espejo apagado: la imagen existente se reorienta (el
  panel re-mapea su propia RAM); con el espejo encendido se repinta.
