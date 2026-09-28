# Entrenar "Hey Sokari"

Sokari detecta su palabra en tu PC, con un puerto a C de [openWakeWord](https://github.com/dscripka/openWakeWord). El detector tiene dos partes:

- **La parte común** (`res/wakeword.bin`): convierte el audio en rasgos. Es igual para cualquier palabra y ya viene en el repo.
- **El clasificador de la palabra** (`res/hey_sokari.jww`): es lo único que se entrena. Si el archivo no existe, el exe se compila sin palabra y a Sokari se le habla con **Ctrl+Alt+J**.

## El modelo que trae Sokari 2.3.0 (beta)

Se entrenó en una PC sin GPU y sin Hugging Face, con los scripts de `entreno_local/`. **Todos los "hey sokari" son voces sintéticas**: nadie real lo dijo.

**Con qué se entrenó**

- **"Hey sokari":**
  - 6 000 clips de LibriTTS-R (800 hablantes, mezclados de a dos). La mitad lo pronuncia a la española: o y a limpias, r suave.
  - 2 550 clips de otros sintetizadores en español: MBROLA (la voz mexicana mx2 y cuatro de España), espeak y la voz "carlfm" de Piper.
  - Cada uno se mezcla dos veces con eco de cuartos, ruido, música o gente hablando.
- **Lo que NO es "hey sokari":**
  - ~24 h de audio real: FLEURS en español latino (9 h) y en inglés (7.6 h), palabras sueltas de Speech Commands, sonidos de casa de ESC-50, música de juegos y ruidos.
  - 7 650 frases parecidas con las mismas voces sintéticas, como "hey safari" u "oye socorro". Ninguna trae "sokari", para que no aprenda a rechazar tu forma de decirlo.
- **Rasgos:** se calculan en bloques de 80 ms, igual que Sokari en vivo.
- **Modelo:** el "dnn" de openWakeWord con 64 neuronas.
- **Cómo se eligió:** con datos de validación aparte. La prueba final se corrió una sola vez.

**Prueba final** (voces y audio que el entrenamiento nunca vio, umbral de fábrica)

| Qué | Resultado |
|---|---|
| Activaciones falsas en 5.8 h de audio real | **0.35 por hora** (2: una en habla en inglés, una en español; ninguna en ruidos de casa, música ni palabras sueltas) |
| Voces nuevas de LibriTTS (silencio / con ruido) | **99 % / 94 %** |
| Variantes nuevas de espeak en español | **94 % / 84 %** |
| Voz mexicana MBROLA mx1 | **57 % / 48 %** (la meta era 70 %: por eso es beta) |
| Frases parecidas que lo activan | 2–9 % |

Con solo 2 activaciones falsas medidas, el valor real puede andar entre ~0.05 y ~1.2 por hora.

**La sensibilidad** (Configuración → Voz y audio) mueve el umbral. Medido en la misma prueba:

| Sensibilidad | Activaciones falsas | Voz mexicana (con ruido / en silencio) |
|---|---|---|
| 50 | 0 por hora | 46 % / 56 % |
| **67 (de fábrica)** | **0.35 por hora** | **48 % / 58 %** |
| 80 | 0.69 por hora | 51 % / 60 % |
| 100 | 1.04 por hora | 57 % / 65 % |

Subirla ayuda poco y multiplica las activaciones falsas.

**Lo que no se sabe:** qué tan bien te oye **a ti**. Ninguna voz sintética lo mide. Si no te oye bien, lo que sirve es reentrenarlo con 20–30 grabaciones de tu voz diciendo "Hey Sokari" con tu micrófono.

## Volver a entrenarlo

- **`entreno_local/`:** los scripts exactos con los que se hizo el modelo incluido. Ver su `LEEME.md`.
- **Cuaderno de Colab** (`entrenar_hey_sokari.ipynb`): la receta original de openWakeWord, con GPU y con sus 2 000 h de negativos en Hugging Face. Nunca se ha corrido completo.
  1. Ábrelo en [Google Colab](https://colab.research.google.com): *Archivo → Subir cuaderno*.
  2. Activa la GPU: *Entorno de ejecución → Cambiar tipo de entorno de ejecución → T4 GPU*.
  3. Dale *Entorno de ejecución → Ejecutar todo*. Tarda más o menos 1–2 horas.
  4. Al final se descarga `hey_sokari.jww`. Súbelo a `res/` en una rama nueva y abre el PR.
  5. Si una celda sale en rojo, copia el mensaje completo para corregir el cuaderno.

## Archivos

| Archivo | Qué es |
|---|---|
| `entreno_local/` | Scripts del entrenamiento local (datos, voces, rasgos, entrenamiento y evaluación) |
| `entrenar_hey_sokari.ipynb` | El cuaderno de Colab. Se genera con `armar_cuaderno.py`: no lo edites a mano. |
| `armar_cuaderno.py` | Arma el cuaderno y le mete el convertidor adentro. |
| `onnx_a_jww.py` | Pasa el modelo entrenado (`.onnx`) al formato de Sokari. Se revisa solo contra onnxruntime y, si no dan lo mismo, no escribe nada. |

Para convertir a mano: `pip install numpy onnx onnxruntime` y luego `python onnx_a_jww.py hey_sokari.onnx hey_sokari.jww`.

## Licencias

- **Código de openWakeWord:** Apache 2.0.
- **Sus modelos preentrenados:** según su README, **todos** están bajo [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/), porque se entrenaron con datos de licencia restrictiva o desconocida.
  - Esto incluye la parte común que usa Sokari (`wakeword.bin`).
  - El modelo de embeddings viene de uno de Google con licencia Apache 2.0, pero openWakeWord publica el suyo bajo CC BY-NC-SA.
- **`hey_sokari.jww`:** se entrena encima de esa parte común, así que se trata igual: CC BY-NC-SA 4.0.
  - Uso no comercial.
  - Hay que dar crédito a openWakeWord.
  - Si alguien lo modifica, tiene que compartirlo con la misma licencia.
- **Datos con que se entrenó** (no vienen en el repo):
  - [FLEURS](https://arxiv.org/abs/2205.12446) y [Speech Commands](https://arxiv.org/abs/1804.03209), de Google: CC BY 4.0.
  - [ESC-50](https://github.com/karolpiczak/ESC-50), de Karol J. Piczak: CC BY-NC 3.0.
  - Voces sintéticas:
    - [piper-sample-generator](https://github.com/rhasspy/piper-sample-generator), con un modelo entrenado sobre LibriTTS-R (CC BY 4.0);
    - la voz "carlfm" de Piper, con datos de dominio público;
    - las voces [MBROLA](https://github.com/mbrola/mbrola-voices), de uso no comercial;
    - espeak-ng.
  - Música de Battle for Wesnoth, Extreme Tux Racer y Frozen Bubble (licencias libres), solo como ruido de fondo.
- **Los audios de `tests/datos/`** salieron de piper-sample-generator (LibriTTS-R, CC BY 4.0).

Por eso Sokari es gratis y no comercial.

# Los íconos

Los íconos de Windows (`res/sokari.ico`), Linux (`res/sokari.png` y `linux/iconos/`) y la app del celular (`movil/assets/` y los de `movil/android/.../res/`) son la esfera con ojos de fábrica, dibujada con el mismo código de la app. Si cambia su cara, se rehacen todos en Linux, desde la raíz del repo (necesita numpy y Pillow):

```sh
python3 herramientas/iconos.py
```

`render_icono.c` dibuja la esfera y `iconos.py` arma cada tamaño: en los chicos la esfera sale más gruesa, sin brillo de más, para que se sigan viendo los ojos.
