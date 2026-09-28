#!/usr/bin/env python3
"""Los íconos de Sokari (Windows, Linux y la app del celular): su esfera con
ojos, dibujada con el mismo código que la app (la cara neutral del estilo
«solo ojos»), sobre un núcleo oscuro para que se vea en barras claras y
oscuras. Cada tamaño se dibuja aparte: en los chicos la esfera sale más
gruesa y se sigue viendo.

Uso, en Linux y desde la raíz del repo (necesita numpy y Pillow):
    python3 herramientas/iconos.py
"""
import io
import math
import os
import struct
import subprocess
import tempfile

import numpy as np
from PIL import Image

HELPER = "build-linux/herramientas/render_icono"
T = 1.3          # el momento del movimiento de la esfera que sale en el ícono
CROP = 0.72      # qué parte del lienzo se queda: la esfera ocupa ~88 % del ícono
CORE = 0.41      # radio del núcleo oscuro (fracción del ícono)
CORE_IN = np.array([0x24, 0x1D, 0x3A]) / 255.0
CORE_OUT = np.array([0x14, 0x12, 0x1B]) / 255.0  # el fondo de la app del celular
ANDROID = "movil/android/app/src/main/res"


def render(canvas, tmp):
    """La esfera en un lienzo de ese lado: RGBA premultiplicado (0..1)."""
    out = subprocess.run([HELPER, str(canvas), str(T), tmp], check=True, capture_output=True, text=True)
    n = int(out.stdout)
    raw = np.frombuffer(open(tmp, "rb").read(), np.uint8).reshape(n, n, 4)
    return raw[..., [2, 1, 0, 3]].astype(np.float32) / 255.0


def resize(pm, size):
    """Premultiplicado: se promedia tal cual, sin orillas oscuras."""
    n = pm.shape[0]
    side = int(round(n * CROP))
    o = (n - side) // 2
    pm = pm[o:o + side, o:o + side]
    chans = [np.asarray(Image.fromarray(pm[..., i]).resize((size, size), Image.LANCZOS)) for i in range(4)]
    return np.clip(np.stack(chans, -1), 0, 1)


def blur(pm, sigma):
    r = max(1, int(3 * sigma))
    x = np.arange(-r, r + 1)
    k = np.exp(-x * x / (2 * sigma * sigma))
    k /= k.sum()
    for ax in (0, 1):
        pad = [(0, 0)] * 3
        pad[ax] = (r, r)
        p = np.pad(pm, pad)
        pm = sum(k[i] * np.take(p, np.arange(i, i + pm.shape[ax]), axis=ax) for i in range(2 * r + 1))
    return pm


def core(size):
    y, x = np.mgrid[0:size, 0:size] + 0.5
    d = np.hypot(x - size / 2, y - size / 2) / size
    a = np.clip((CORE - d) * size + 0.5, 0, 1)[..., None]  # borde suave de un píxel
    t = np.clip(d / CORE, 0, 1)[..., None]
    return np.concatenate([(CORE_IN * (1 - t) + CORE_OUT * t) * a, a], -1)


def icon(size, tmp):
    """Puntos nítidos + un poco del resplandor de la esfera en la app. En los
    chicos el renderizador ya la dibuja gruesa: sin brillo de más (si no, el
    anillo se vuelve blanco y se come los ojos)."""
    if size <= 32:
        gain, soft_w, bloom_w, k = 1.0, 0.0, 0.0, 4
    elif size <= 64:
        gain, soft_w, bloom_w, k = 1.2, 0.25, 0.6, 2
    else:
        gain, soft_w, bloom_w, k = 1.4, 0.5, 1.2, 2
    crisp = resize(render(math.ceil(k * size / CROP), tmp), size)
    sphere = gain * crisp
    if soft_w:
        sphere = sphere + soft_w * resize(render(math.ceil(0.5 * size / CROP), tmp), size)
    if bloom_w:
        sphere = sphere + bloom_w * blur(crisp, 0.01 * size)
    sphere = np.clip(sphere, 0, 1)
    pm = sphere + core(size) * (1 - sphere[..., 3:4])
    a = pm[..., 3:4]
    rgb = np.where(a > 0, pm[..., :3] / np.maximum(a, 1e-6), 0)
    return Image.fromarray((np.concatenate([rgb, a], -1).clip(0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def centered(im, size):
    """Para el ícono adaptable de Android: al centro de un lienzo transparente."""
    out = Image.new("RGBA", (size, size))
    out.alpha_composite(im, ((size - im.width) // 2, (size - im.height) // 2))
    return out


def png_bytes(im):
    buf = io.BytesIO()
    im.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def save_ico(path, images):
    """ICO con cada tamaño en PNG (como el de antes; Windows Vista en adelante)."""
    blobs = [png_bytes(im) for im in images]
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for im, blob in zip(images, blobs):
        side = im.width if im.width < 256 else 0
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    open(path, "wb").write(head + entries + b"".join(blobs))


def main():
    subprocess.run(["make", "-f", "Makefile.linux", "-s", HELPER], check=True)
    fd, tmp = tempfile.mkstemp(suffix=".raw")
    os.close(fd)
    try:
        cache = {}

        def get(size):
            if size not in cache:
                cache[size] = icon(size, tmp)
            return cache[size]

        sizes = [16, 24, 32, 48, 64, 128, 256]
        save_ico("res/sokari.ico", [get(s) for s in sizes])
        get(256).save("res/sokari.png", optimize=True)
        for s in sizes:
            get(s).save(f"linux/iconos/{s}.png", optimize=True)
        # El celular: el ícono de siempre y el adaptable (el fondo #14121B lo pone
        # Android; la esfera, más chica, cabe en cualquier forma de ícono).
        get(512).save("movil/assets/icono.png", optimize=True)
        centered(get(336), 432).save("movil/assets/icono_primer_plano.png", optimize=True)
        for dpi, legacy in [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]:
            get(legacy).save(f"{ANDROID}/mipmap-{dpi}/ic_launcher.png", optimize=True)
            fg = legacy * 9 // 4  # 108 dp de 48
            centered(get(round(fg * 336 / 432)), fg).save(f"{ANDROID}/drawable-{dpi}/ic_launcher_foreground.png",
                                                          optimize=True)
    finally:
        os.remove(tmp)
    print("listo: res/sokari.ico, res/sokari.png, linux/iconos/, movil/assets/ y los de Android")


if __name__ == "__main__":
    main()
