# ─────────────────────────────────────────────────────────────────────────
# sd/ — el contenido de la tarjeta SD del proyecto
#
#   image/  las fotos originales, tal cual cayeron del movil.  NO se tocan:
#           son la fuente.  Nombres con espacios y parentesis, sin problema:
#           aqui solo las lee ImageMagick.
#   img/    la version de vision, reducida al panel: p01.jpg, p02.jpg...
#   thm/    las miniaturas para la rejilla de la galeria: t01.jpg, t02.jpg...
#   java/   los .class que instalan las apps (`make send' en apps/java/*)
#
# `make' regenera img/ y thm/ desde image/.  `make push' las sube a la placa
# por el REST del httpd (PUT /api/file), que es como se llena la tarjeta sin
# sacarla.
#
# Por que se reducen: el decodificador de LVGL (sjpg) reserva la imagen
# ENTERA en PSRAM cuando decodifica un JPEG --ancho x alto x 3 bytes, ver
# decoder_open en lvgl/src/extra/libs/sjpg/lv_sjpg.c --, asi que un 1920x2560
# originals serian 14 MB de cache y un refresco de varios segundos por SPI2.
# Reducidas a 310x400 son ~370 KB y se ven en el sitio.
#
# El numero de cada foto es su posicion en el orden alfabético de image/, y
# img/pNN.jpg va siempre con thm/tNN.jpg: la galeria lista /img y cambia el
# prefijo para sacar la miniatura.
# ─────────────────────────────────────────────────────────────────────────

BOARD    ?= 192.168.1.146
# ImageMagick 7 pide `magick`; `convert` sigue funcionando pero avisa de que
# esta depreciado.
CONVERT  ?= magick

VISTA    = 310x400
# La miniatura es CUADRADA y recortada por el centro, no "encajada dentro de
# una caja".  Con `-resize 96x128` a secas ImageMagick mete la foto dentro del
# rectangulo conservando la proporcion, asi que de las 88 originales 80 salian
# de 96x128 pero 7 de 96x72 y 1 de 72x128 (medido con `identify` sobre thm/):
# celdas con hueco alrededor y tamales desiguales, que es el "no se ven bien
# algunas" que se reporto.  Con `^` la imagen se escala hasta CUBRIR el
# rectangulo y `-extent` la recorta por el centro: todas salen exactamente del
# tamano de la celda de la rejilla.
MINIATURA = 96x96

.PHONY: all push clean

all:
	@command -v $(CONVERT) >/dev/null 2>&1 || { \
		echo "ERROR: hace falta ImageMagick ('$(CONVERT)')."; exit 1; }
	@mkdir -p img thm
	@i=1; for f in image/*.jpg image/*.jpeg; do \
		[ -e "$$f" ] || continue; \
		n=`printf '%02d' $$i`; \
		$(CONVERT) "$$f" -auto-orient -resize $(VISTA)         -quality 82 img/p$$n.jpg || exit 1; \
		$(CONVERT) "$$f" -auto-orient -resize $(MINIATURA)^ -gravity center -extent $(MINIATURA) -quality 80 thm/t$$n.jpg || exit 1; \
		i=`expr $$i + 1`; \
	done; echo "  [ok]    `expr $$i - 1` fotos -> img/ ($(VISTA)) y thm/ ($(MINIATURA))"

# El httpd del P4 atiende una conexion a la vez y el body se lee con un
# TCP_CTRL_RCVTIMEO de 2 s (shell/xsh_httpd.c:50).  Si el kernel esta ocupado
# --un refresco de JPEG por SPI2 tarda 1,6 s--, el read() corta antes de
# terminar el fichero y la API responde 400 "incomplete body", borrando lo
# que llevaba escrito.  Es transitorio y el PUT es idempotente, asi que se
# reintenta.
CURL      = curl -fsS --retry 4 --retry-delay 2 --retry-all-errors --max-time 90

push:
	@for f in img/*.jpg; do \
		b=`basename $$f`; \
		$(CURL) -T "$$f" "http://$(BOARD)/api/file?path=/img/$$b" || exit 1; \
		echo "  [ok]    $$f -> $(BOARD):/img/$$b"; \
	done
	@for f in thm/*.jpg; do \
		b=`basename $$f`; \
		$(CURL) -T "$$f" "http://$(BOARD)/api/file?path=/thm/$$b" || exit 1; \
		echo "  [ok]    $$f -> $(BOARD):/thm/$$b"; \
	done
	@echo "  [ok]    `ls img/*.jpg | wc -l` en /img, `ls thm/*.jpg | wc -l` en /thm"

clean:
	@$(RM) -r img thm
