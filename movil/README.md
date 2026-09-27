# Sokari en el celular

Una app de Android para hablarle a **tu Sokari de la PC** desde el celular, en casa o fuera. Tocas el botón, hablas, y tu PC hace el trabajo: abre apps, pone música, busca, te recuerda cosas. La respuesta se lee en voz alta en el celular.

El celular no ejecuta nada por su cuenta:

1. Pasa tu voz a texto con el reconocimiento de voz de Android.
2. Se lo manda a tu PC por [Tailscale](https://tailscale.com).
3. Te lee lo que contesta.

## Qué necesitas

- Sokari abierto en tu PC (versión 2.2.0 o más nueva).
- Tailscale en la PC y en el celular, con la misma cuenta.
- El secreto de malla de tu PC: en Sokari, *Configuración → General*, campo "Tus otros dispositivos (Tailscale) — secreto de malla".
  - Sokari lo crea solo la primera vez que arranca con Tailscale conectado.
  - Tiene 64 caracteres: pásatelo al celular copiado, o cámbialo por uno tuyo (el mismo en todas tus PCs).
  - Ahí mismo sale la IP de Tailscale de tu PC (100.x.y.z).

## Instalar

1. En GitHub, en **Releases**, abre la versión más nueva y baja **`Sokari.apk`**. Va junto a `Sokari.exe` y lleva su misma versión (Sokari 2.3.0 → app 2.3.0).
2. Ábrelo en el celular. Android te va a pedir permiso para instalar apps de esa fuente.
3. Abre **Sokari**, toca *Configurar* y pon:
   - la IP de tu PC (o su nombre `.ts.net`);
   - el secreto de malla.
4. Dale **Probar conexión** y luego **Guardar**.

La prueba de conexión no gasta tu cupo de Groq: solo revisa que tu PC conteste y que el secreto coincida.

**Para actualizar**, instala el `Sokari.apk` nuevo encima. Todas las versiones van firmadas con la misma llave, así que conserva la IP y el secreto.

**Una sola vez:** si antes instalaste un APK de prueba (de la pestaña *Actions*), desinstálalo antes de instalar el del release. Los de prueba van firmados con otra llave y Android no deja instalar uno encima del otro.

## Uso

- Al abrirla empieza a escuchar; se puede apagar en Configuración. Toca el botón para hablar, otra vez para cortar, y mientras lee la respuesta, para callarla.
- También puedes escribirle.
- Puedes elegirla como asistente del celular (*Ajustes → Apps → Apps predeterminadas → Asistente digital*, según tu celular). Así se abre manteniendo presionado el botón de inicio o de encendido.

## Si no conecta

| Mensaje | Qué hacer |
|---|---|
| "No encuentro tu PC" | Revisa que la PC esté prendida, con Sokari abierto, y Tailscale conectado en los dos. Si conectaste Tailscale después de abrir Sokari, reinícialo: su servidor para el celular arranca al abrirse. |
| "El secreto no coincide" | Copia otra vez el secreto de *Configuración → General* en la PC. |
| "Sokari está ocupado" | Estaba contestándote a ti o a otra PC; prueba en unos segundos. |

## Si se queda en el ícono de Sokari

Android muestra el ícono de Sokari mientras la app dibuja su primera pantalla. Si en tu celular no alcanza a dibujarla, la app ya no se queda ahí para siempre:

1. A los 10 segundos sale un aviso, **Sokari no pudo mostrar su pantalla**, con el detalle: tu celular, tu versión de Android y el modo de dibujo que falló.
2. **Reintentar** vuelve a abrir la app con otro modo de dibujo. Primero prueba Impeller con OpenGL y luego Skia. El modo que funcione se queda en tu celular, y con cada versión nueva de la app se vuelve a probar el normal.
3. Si sigue igual, toca **Copiar** y manda ese texto a quien te ayuda con Sokari.

Si la app se cerró por un error, la siguiente vez que la abras te lo muestra arriba, con **Copiar**.

## Límites y privacidad

- **Lo delicado no se puede confirmar desde el celular.** Mover o borrar archivos pide un "sí" de voz frente a la PC.
- **Si la PC está apagada, no hay Sokari.**
- **Tu voz** la pasa a texto el reconocimiento de voz de Android, normalmente el de Google. Según el celular puede hacerse en el propio celular o en sus servidores.
- **El texto** va de tu celular a tu PC por Tailscale, cifrado. Tu PC lo manda a Groq, como cuando le hablas directo.
- **Dónde se conecta:** solo a direcciones de Tailscale (100.64.0.0/10 o nombres `.ts.net`, y solo si resuelven a una IP de Tailscale). Nunca manda el secreto a otro lado.
- **Dónde guarda tus datos:** la dirección y el secreto se guardan cifrados con la llave del sistema (Android Keystore).
- **Sin "Hey Sokari" en el celular.** Oír todo el tiempo gasta batería, y Android obliga a una notificación fija.

## La llave de firma (una sola vez, para publicar)

Para que cada versión se instale encima de la anterior, el APK del release se firma siempre con la misma llave. Vive en dos secretos del repo:
- `ANDROID_LLAVE`: la llave en base64;
- `ANDROID_LLAVE_CLAVE`: su contraseña.

Se agregan en *Settings → Secrets and variables → Actions → New repository secret*.

- **Si faltan**, el release sale sin `Sokari.apk` y lo avisa en sus notas: nunca publica un APK que obligue a desinstalar. En las ramas, el CI prueba la firma con una llave temporal.
- **Si pierdes la llave**, crea una nueva y cambia los dos secretos. La siguiente versión habrá que instalarla desinstalando la anterior una vez.

Para crear una:

```
keytool -genkeypair -storetype PKCS12 -keystore sokari.p12 -alias sokari -keyalg RSA -keysize 2048 -validity 10950 -dname "CN=Sokari"
```

`keytool` viene con Java y con Android Studio. Luego, en PowerShell, `[Convert]::ToBase64String([IO.File]::ReadAllBytes("sokari.p12"))` da el valor de `ANDROID_LLAVE`.

## Para desarrollar

Flutter 3.47.5:

```
cd movil
flutter pub get
flutter analyze
flutter test
flutter build apk --release
```

| Archivo | Qué hace |
|---|---|
| `lib/malla.dart` | Habla con la PC: valida que la dirección sea de Tailscale, manda el pedido y traduce los errores |
| `lib/voz.dart` | Reconocimiento de voz y lectura en voz alta de Android |
| `lib/ajustes.dart` | Dirección, secreto y preferencias, guardados cifrados |
| `lib/main.dart`, `lib/pantalla_ajustes.dart` | Las dos pantallas |
| `lib/fallas.dart` | Los errores a la vista, con «Copiar» (nada se queda callado) |
| `android/.../MainActivity.kt`, `Arranque.kt`, `AvisoActivity.kt` | Si no dibuja en 10 s: el aviso de Android y el modo de dibujo de respaldo |
| `probar_emulador.sh` | La CI instala el APK en un Android emulado (API 29 y 34) y revisa que dibuje con cada modo y que «Reintentar» funcione |
| `test/` | Pruebas del cliente (con una PC de mentira) y de las pantallas (con micrófono y voz de mentira) |

El ícono sale de `assets/`: `dart run flutter_launcher_icons`.
