# Compila Sokari.exe (un solo ejecutable estático, sin DLLs extra).
# Requiere MinGW-w64 (por ejemplo WinLibs: winget install BrechtSanders.WinLibs.POSIX.UCRT)
# y correrse desde un shell tipo POSIX (Git Bash):  mingw32-make

CC = gcc
WINDRES = windres

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 \
          -DCOBJMACROS -Isrc -MMD -MP
LIBS := -lwinhttp -lole32 -loleaut32 -luuid -lwinmm -lgdi32 -luser32 -lshell32 -lshlwapi \
        -lcomctl32 -ldwmapi -lws2_32 -liphlpapi -lbcrypt -ladvapi32 -lcomdlg32 -luxtheme -lpowrprof -lmfplay -lmfplat -lm

# Detector de voz de WebRTC (C puro, licencia BSD): ver src/third_party/webrtc_vad.
VAD_SRC := $(wildcard src/third_party/webrtc_vad/webrtc/common_audio/vad/*.c) \
           $(wildcard src/third_party/webrtc_vad/webrtc/common_audio/signal_processing/*.c)
SRC := $(filter-out src/main.c,$(wildcard src/*.c)) src/third_party/cJSON.c $(VAD_SRC)
OBJ := $(patsubst src/%.c,build/obj/%.o,$(SRC))
MAIN_OBJ := build/obj/main.o
RES_OBJ := build/obj/sokari_res.o
# El modelo de "Hey Sokari" entra al exe solo si res/hey_sokari.jww existe (lo
# genera el cuaderno de herramientas/). Sin él, se le habla con Ctrl+Alt+J.
HEY_SOKARI := $(wildcard res/hey_sokari.jww)
RES_DEPS := res/sokari.rc res/sokari.manifest res/sokari.ico res/tools.json res/system_prompt.txt res/wakeword.bin \
            $(HEY_SOKARI)

# Con Sokari abierto, Windows no deja reemplazar dist/Sokari.exe:
# mingw32-make OUT=build/Sokari.exe compila en otro lado.
OUT ?= dist/Sokari.exe

# GCC de MinGW enlaza su propio manifiesto (default-manifest.o), que choca con
# el de Sokari (".rsrc merge failure: multiple non-default manifests"). Con -B
# encuentra primero este objeto vacío del mismo nombre y queda un solo manifiesto.
NOMANIFEST_DIR := build/nomanifest/
NOMANIFEST := $(NOMANIFEST_DIR)default-manifest.o

.PHONY: all clean tests

all: $(OUT)

$(OUT): $(OBJ) $(MAIN_OBJ) $(RES_OBJ) | $(NOMANIFEST)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $^ -B$(NOMANIFEST_DIR) -static -municode -mwindows -s $(LIBS)

$(NOMANIFEST):
	@mkdir -p $(dir $@)
	printf '' | $(CC) -x c -c - -o $@

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Código de terceros tal cual: sus avisos no son nuestros (y la CI falla con cualquiera).
build/obj/third_party/webrtc_vad/%.o: CFLAGS += -Isrc/third_party/webrtc_vad -w
build/obj/vad.o: CFLAGS += -Isrc/third_party/webrtc_vad

# Kernels de la red del wake word: una copia con AVX2+FMA y otra genérica;
# wakeword.c elige cuál usar según el CPU al arrancar.
build/obj/nn_avx2.o: CFLAGS += -O3 -mavx2 -mfma
build/obj/nn_generic.o: CFLAGS += -O3

$(RES_OBJ): $(RES_DEPS)
	@mkdir -p $(dir $@)
	$(WINDRES) $(if $(HEY_SOKARI),-DHAVE_HEY_SOKARI) -I res -i res/sokari.rc -o $@

TEST_SRC := $(wildcard tests/*.c)
TESTS := $(patsubst tests/%.c,build/tests/%.exe,$(TEST_SRC))

tests: $(TESTS)

build/tests/%.exe: tests/%.c $(OBJ) $(RES_OBJ) | $(NOMANIFEST)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -B$(NOMANIFEST_DIR) -static -municode -mconsole $(LIBS)

# La prueba de confirmación trae su propio Groq y su propio run_tool de
# mentira, así que se enlaza sin groq.o ni tools.o.
build/tests/test_confirmacion.exe: tests/test_confirmacion.c $(filter-out build/obj/groq.o build/obj/tools.o,$(OBJ)) $(RES_OBJ) | $(NOMANIFEST)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -B$(NOMANIFEST_DIR) -static -municode -mconsole $(LIBS)

clean:
	rm -rf build dist/Sokari.exe

-include $(OBJ:.o=.d) $(MAIN_OBJ:.o=.d)
