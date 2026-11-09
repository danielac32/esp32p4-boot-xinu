# ─────────────────────────────────────────────────────────────────────────
# java/sdk/app.mk — reglas comunes de cada app
#
# Lo include el Makefile de la app, despues de definir APP:
#
#     APP = Calculadora
#     include ../../../java/sdk/app.mk
#
# El SDK se recompila aqui en cada `make' (son 7 ficheros declarativos y el
# coste es un segundo de javac) en lugar de depender de que alguien haya
# ejecutado antes make en java/sdk: asi el build de una app es independiente
# del estado del arbol.  Los .class del SDK van al mismo build/ de la app
# porque el javac de la app los necesita como classpath; a la placa no se
# envian, solo el .class de la app.
#
# Variables que la app puede pisar antes del include:
#   JAVAC   compilador (por defecto javac del PATH)
#   BUILD   carpeta de salida (por defecto build)
#   BOARD   IP de la placa para `make send' (por defecto 192.168.1.146)
#   SDJava  carpeta de destino en la tarjeta (por defecto /java)
# ─────────────────────────────────────────────────────────────────────────

JAVAC    ?= javac
RELEASE   = 8
BUILD    ?= build
BOARD    ?= 192.168.1.146
SDJAVA   ?= /java

SDK_DIR  ?= ../../../java/sdk
SDK_SRC   = $(wildcard $(SDK_DIR)/esp32p4/*.java)

.PHONY: all send clean help

all: $(BUILD)/$(APP).class
	@echo "  [ok]    $(APP) -> $(BUILD)/$(APP).class"

$(BUILD)/$(APP).class: $(APP).java $(SDK_SRC)
	@command -v $(JAVAC) >/dev/null 2>&1 || { \
		echo "ERROR: no se encuentra '$(JAVAC)'; instala un JDK."; \
		exit 1; }
	@mkdir -p $(BUILD)
	@$(JAVAC) --release $(RELEASE) -Xlint:-options -d $(BUILD) $(SDK_SRC)
	@$(JAVAC) --release $(RELEASE) -Xlint:-options -cp $(BUILD) -d $(BUILD) $(APP).java

# Sube el .class a la placa por el REST del httpd (PUT /api/file crea los
# directorios que falten).  Requiere que la placa este arrancada y en la red.
# El reintento es por el RCVTIMEO de 2 s del httpd: si el kernel esta pintando
# un JPEG (1,6 s por SPI2) el read() corta el body y la API responde 400.
#
# Si hay un $(APP).txt junto al fuente se sube tambien: es la descripcion que
# lee el launcher (apps/java/workspace) para la tarjeta de la app.
send: all
	@curl -fsS --retry 4 --retry-delay 2 --retry-all-errors --max-time 90 \
		-T $(BUILD)/$(APP).class \
		"http://$(BOARD)/api/file?path=$(SDJAVA)/$(APP).class"
	@test ! -f $(APP).txt || curl -fsS --retry 4 --retry-delay 2 \
		--retry-all-errors --max-time 90 \
		-T $(APP).txt "http://$(BOARD)/api/file?path=$(SDJAVA)/$(APP).txt"
	@echo "  [ok]    $(APP).class -> $(BOARD):$(SDJAVA)/"

clean:
	@$(RM) -r $(BUILD)

help:
	@echo "$(APP)"
	@echo "  make        compila -> $(BUILD)/$(APP).class"
	@echo "  make send   compila y sube el .class a $(BOARD):$(SDJAVA)/"
	@echo "  make clean  borra $(BUILD)/"
	@echo ""
	@echo "  En la placa:  java $(APP)   (o `ps'/'kill' para ver la tarea)"
