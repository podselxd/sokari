#!/bin/sh
# El APK de verdad en un Android emulado (la CI lo corre, ver
# .github/workflows/movil.yml). Tiene que pasar del ícono de Sokari a su
# pantalla: Android lo avisa en el registro con «primera pantalla dibujada»
# (MainActivity.kt). Se prueba:
#   1. cada modo de dibujo: normal, Impeller con OpenGL y Skia;
#   2. el respaldo completo: el aviso de Android sale, «Reintentar» vuelve a
#      abrir la app y ya dibuja con el modo siguiente.
# Las capturas y los registros quedan en la carpeta de salida.
# Uso: sh probar_emulador.sh Sokari.apk carpeta_de_salida
set -u
apk=$1
out=${2:-emulador}
paquete=com.podsel.sokari_remoto
mkdir -p "$out"
fallas=0

falla() {
    echo "::error::$1"
    fallas=1
}

# Espera a que el registro traiga el texto (hasta $2 segundos).
esperar() {
    i=0
    while [ "$i" -lt "$2" ]; do
        if adb logcat -d -s Sokari:I | grep -q "$1"; then return 0; fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

limpiar() {
    adb shell am force-stop "$paquete"
    adb shell pm clear "$paquete" >/dev/null
    adb logcat -c
}

adb install -r "$apk" >/dev/null || { echo "::error::No se pudo instalar el APK en el emulador."; exit 1; }
adb shell getprop ro.build.version.release | sed 's/^/Android /'

# 1. Cada modo de dibujo.
for modo in 0 1 2; do
    limpiar
    adb shell am start -W -n "$paquete/.MainActivity" --ei modo_dibujo "$modo" >/dev/null
    if esperar "primera pantalla dibujada" 60; then
        sleep 2
        motor=$(adb logcat -d | grep -oE "Using the Impeller rendering backend \([^)]*\)" | head -1)
        echo "Modo $modo: dibujó su pantalla. $(adb logcat -d -s Sokari:I | grep -o 'modo de dibujo: [^)]*' | head -1); motor: ${motor:-sin Impeller}"
        if [ "$modo" = 1 ] && ! echo "$motor" | grep -q OpenGLES; then
            falla "Modo 1 debía dibujar con Impeller y OpenGL, y usó: ${motor:-otro}"
        fi
        if [ "$modo" = 2 ] && [ -n "$motor" ]; then
            echo "::warning::Modo 2 (Skia) igual usó Impeller: en esta versión de Flutter no se puede apagar."
        fi
    else
        falla "Modo $modo: no dibujó su pantalla en 60 s."
    fi
    adb exec-out screencap -p > "$out/modo$modo.png"
    adb logcat -d > "$out/registro_modo$modo.txt"
    grep -E "AndroidRuntime|FATAL|F flutter|E flutter" "$out/registro_modo$modo.txt" | tail -20
done

# Lo que ve quien la abre por primera vez (en el último modo).
adb shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 && adb pull /sdcard/ui.xml "$out/pantalla.xml" >/dev/null 2>&1
if grep -q "Conecta tu PC" "$out/pantalla.xml" 2>/dev/null; then
    echo "Se ve «Conecta tu PC»."
else
    echo "::warning::No encontré «Conecta tu PC» en el árbol de la pantalla (revisa las capturas)."
fi

# 2. El respaldo completo: el aviso y «Reintentar».
limpiar
adb shell am start -W -n "$paquete/.MainActivity" --ez probar_aviso true >/dev/null
visto=0
i=0
while [ "$i" -lt 30 ]; do
    adb shell uiautomator dump /sdcard/aviso.xml >/dev/null 2>&1
    adb pull /sdcard/aviso.xml "$out/aviso.xml" >/dev/null 2>&1
    if grep -q "Sokari no pudo mostrar su pantalla" "$out/aviso.xml" 2>/dev/null; then visto=1; break; fi
    sleep 1
    i=$((i + 1))
done
adb exec-out screencap -p > "$out/aviso.png"
if [ "$visto" = 1 ]; then
    echo "El aviso de Android se ve."
    grep -q "Celular: " "$out/aviso.xml" || falla "El aviso no trae los datos del celular."
    caja=$(grep -oiE 'text="Reintentar"[^>]*bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"' "$out/aviso.xml" |
        grep -oE '\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]' | head -1)
    if [ -n "$caja" ]; then
        # shellcheck disable=SC2046 # se parte a propósito: x1 y1 x2 y2
        set -- $(echo "$caja" | tr '[],' '   ')
        adb logcat -c
        adb shell input tap $(((${1} + ${3}) / 2)) $(((${2} + ${4}) / 2))
        if esperar "primera pantalla dibujada (modo de dibujo: Impeller con OpenGL)" 60; then
            echo "«Reintentar» volvió a abrir la app y dibujó con el modo de respaldo."
        else
            falla "Después de «Reintentar» no dibujó con el modo de respaldo."
        fi
        adb exec-out screencap -p > "$out/reintento.png"
        adb logcat -d > "$out/registro_reintento.txt"
    else
        falla "No encontré el botón «Reintentar» en el aviso."
    fi
else
    falla "El aviso de Android no apareció."
fi
adb logcat -d > "$out/registro_aviso.txt"
exit $fallas
