# ─────────────────────────────────────────────────────────────────────────
# java/sdk — el SDK java del ESP32-P4
#
# Son las DECLARACIONES de las natives que implementa el kernel
# (device/jvm/native_*.c, registradas en la tabla de jvm/java_lib.c).  Se
# compilan aqui solo para dar classpath al `javac` de las apps: en la placa no
# hacen falta, porque el interprete resuelve las natives por nombre de
# clase+metodo contra su tabla, no por .class.
#
# Las apps usan las reglas comunes de app.mk (include), que compila el SDK por
# su cuenta; este Makefile es para cuando se quiere comprobar el SDK solo.
# ─────────────────────────────────────────────────────────────────────────

JAVAC   ?= javac
RELEASE  = 8
BUILD   ?= build
SRC_DIR  = esp32p4
SRC      = $(wildcard $(SRC_DIR)/*.java)

.PHONY: all clean help

all: $(BUILD)/$(SRC_DIR)/LVGL.class
	@echo "  [ok]    SDK -> $(BUILD)/$(SRC_DIR)/  ($(words $(SRC)) clases)"

$(BUILD)/$(SRC_DIR)/LVGL.class: $(SRC)
	@command -v $(JAVAC) >/dev/null 2>&1 || { \
		echo "ERROR: no se encuentra '$(JAVAC)'; instala un JDK."; \
		exit 1; }
	@mkdir -p $(BUILD)
	@$(JAVAC) --release $(RELEASE) -Xlint:-options -d $(BUILD) $(SRC)

clean:
	@$(RM) -r $(BUILD)

help:
	@echo "java/sdk"
	@echo "  make        compila las clases del SDK a $(BUILD)/"
	@echo "  make clean  borra $(BUILD)/"
	@echo ""
	@echo "  Clases: LVGL (UI), FAT (SD), HW (tiempo/gpio/audio),"
	@echo "          IO (consola), Sys (tareas), Net (IP), RGB (sin LED en el P4)"
