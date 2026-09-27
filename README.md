# Sokari

Asistente de voz personal para Windows (y, en beta, para Linux). Le dices **"Hey Sokari"** y te contesta con voz. Puede abrir apps, buscar en internet, controlar tu música y tus ventanas, manejar archivos, recordarte cosas y hablar con tus otras PCs.

Es **un solo `Sokari.exe`** de unos 3 MB, escrito en C. No necesita Python ni instaladores, y no usa archivos `.bat` ni DLLs extra.

## Instalación

1. Baja `Sokari.exe` de la [última versión](https://github.com/podselxd/sokari/releases/latest) y déjalo en la carpeta que quieras.
2. Ábrelo. La primera vez te pide tu API key de Groq, que es gratis y no pide tarjeta: sácala en [console.groq.com/keys](https://console.groq.com/keys). Tu nombre y lo demás son opcionales.
3. Listo. Sokari dice "en línea" y queda un ícono en la bandeja, junto al reloj. La esfera aparece cuando le hablas.

Cuando lo abres a mano después de la primera vez, sale la **ventana de Inicio** (abajo). Si lo pusiste a iniciar con Windows, al prender la PC arranca directo, sin esa ventana.

Se actualiza solo. Revisa GitHub al arrancar y cada 6 horas, y solo instala la versión nueva cuando no le estás hablando. Antes de reemplazarse comprueba la huella SHA-256 del archivo. Si GitHub no le deja consultar (responde 403 cuando se acaba su límite de consultas para tu red, algo común si tu compañía de internet comparte la IP), lo revisa en la página del release y comprueba la descarga con la huella que se publica junto a cada archivo (`Sokari.exe.sha256`).

### En Linux (beta)

Ubuntu 24.04 o más nuevo, o Fedora 44, con GNOME. **Es beta:** está probado en la CI (GNOME 46 y 50 sin pantalla), todavía no en una PC real. Lo que falle ahí se corrige en la siguiente versión.

1. Baja `Sokari.deb` (Ubuntu) o `Sokari.rpm` (Fedora) de la [última versión](https://github.com/podselxd/sokari/releases/latest).
2. Instálalo desde la carpeta donde quedó (pide tu contraseña):

   ```bash
   sudo apt install ./Sokari.deb    # Ubuntu
   sudo dnf install ./Sokari.rpm    # Fedora
   ```

3. Cierra sesión y vuelve a entrar **una vez**. Así GNOME carga la extensión de Sokari, la que le deja ver tus ventanas y oprimir teclas. `sokari --revisar-gnome` te dice si ya funciona.
4. Abre **Sokari** desde tus apps. La primera vez te pide tu API key de Groq, igual que en Windows.

Su **Configuración** tiene las mismas secciones que la de Windows: Cuenta, Pantalla, Voz y audio, General, Skills, Tus PCs e IA de respaldo. En Linux, **la de fábrica es la esfera flotante**: solo la esfera, transparente y encima de todo; la arrastras a donde quieras y con clic derecho abres el menú. En Pantalla también están los otros modos (pantalla completa, ventana, minimizado) y «Aparecer solo cuando le hablas». En GNOME con Wayland, que la esfera quede encima de todo y regrese a donde la dejaste lo hace la extensión de Sokari: la primera vez, cierra sesión y vuelve a entrar para que GNOME la cargue. Todavía falta en Linux la ventana de Inicio; **Detectar mis PCs** solo encuentra PCs con Windows (las de Linux se registran diciéndole «registra mi laptop en 100.x.y.z»).

No se actualiza solo, porque instalar pide tu contraseña: te avisa cuando sale una versión nueva, y en su menú **Buscar actualización** (o `sokari --actualizar`) la baja, comprueba su huella SHA-256 y la instala. Para quitarlo: `sudo apt remove sokari` o `sudo dnf remove sokari` (tu configuración y tu memoria se quedan en `~/.config/sokari` y `~/.local/share/sokari`).

## Uso

- Di **"Hey Sokari"** y habla. Puedes decirlo todo de corrido ("Hey Sokari, abre Spotify") o hacer una pausa y esperar el tono.
- Te escucha solo mientras hablas, hasta 30 s. Termina 0.8 s después de que te callas, aunque siga el ruido de fondo (un ventilador, un zumbido) o una tele o gente platicando que ya sonaba antes de que le hablaras, si suena bastante más bajito que tú (unos 9 dB o más; si suena casi tan fuerte como tú, sigue escuchando hasta los 30 s). Si te corta a media frase, en Configuración → Voz y audio elige **Más** (1.2 s); si quieres que conteste antes, **Poco** (0.6 s).
- Mientras te escucha, baja el volumen de la PC y luego lo regresa, para que un video o música no tapen tu voz. Se apaga en Configuración → Voz y audio. Si no dijiste nada (solo hubo ruido), no manda nada a transcribir.
- **Ctrl+Alt+J** sirve para hablarle sin decir "Hey Sokari".
- **"Hey Sokari" es beta.** Se entrenó solo con voces sintéticas. En pruebas con voces que nunca oyó:
  - se activó por error 0.35 veces por hora de audio real;
  - detectó el 94 % de las voces en inglés y ~50–60 % de una voz mexicana sintética.
  
  Con tu voz todavía no está medido. Si no te oye, usa **Ctrl+Alt+J**; lo que lo arregla es reentrenarlo con grabaciones de tu voz. Subir la sensibilidad ayuda poco. Detalles en [Entrenar "Hey Sokari"](#entrenar-hey-sokari).
- Después de cada respuesta te sigue escuchando unos segundos, sin que repitas "Hey Sokari".
- Para callarla mientras habla: di **"Hey Sokari"**, aprieta **Ctrl+Alt+J** o dale **un clic a la esfera**. Todo lo que dice se puede saltar, también las explicaciones largas. Si le das clic mientras piensa, no dice la respuesta (lo que ya hizo no se deshace). Con ella quieta, un clic no hace nada, y si arrastras la esfera flotante, no cuenta como clic. Otros ruidos no la interrumpen.
- Para cerrar la conversación dile "adiós", "ya vete", "eso es todo", "hasta mañana", "luego hablamos" o "me voy a dormir". También termina cuando Sokari se despide o si dejas de hablarle. Si configuraste una palabra de apagado y la dices, Sokari se cierra al instante.
- Con **Aparecer solo cuando le hablas** (Configuración → Pantalla, prendida de fábrica), la esfera aparece en tu modo de pantalla al hablarle y se esconde al terminar. Al abrir Sokari se ve y se queda hasta tu primera conversación.
- **Al aparecer y desaparecer** (Configuración → Pantalla), la esfera se anima: **Materializarse** (de fábrica: llega en pedazos y se junta; al irse se dispersa), **Deslizarse** (sube desde abajo de la pantalla y baja al irse), **Zoom** (crece desde un punto y se encoge) o **Ninguna**. Entra en 1 segundo y sale en 0.8. **Probar** te la muestra sin guardar. En Minimizado no hay animación.

Menú del ícono de la bandeja (clic derecho): **Hablar con Sokari**, **Ocultar/Mostrar la esfera**, **Modo de pantalla**, **Silenciar micrófono**, **Acceso completo (menos borrar)**, **Ventana de inicio…**, **⚙ Configuración…**, **Abrir carpeta de Sokari** y **Salir**.

### Ventana de Inicio

Sale al abrir `Sokari.exe` a mano, o desde **Ventana de inicio…** en la bandeja. Tiene:

- **Iniciar Sokari** (o **Mostrar la esfera**, si ya está corriendo). Si la cierras sin iniciar, Sokari se cierra.
- **Modo de pantalla** y **Salida de audio** (bocinas o audífonos). Los cambios se aplican al momento.
- **Configuración**, **Silenciar micrófono**, **Probar audio**, **Buscar actualizaciones**, **Abrir carpeta de datos** y **Salir**.

"Probar audio" hace sonar el tono de Sokari por la salida elegida y, si Sokari ya está iniciado, también su voz.

### Configuración

| Sección | Qué tiene |
|---|---|
| Inicio | Iniciar, modo de pantalla, salida de audio y botones rápidos (ver arriba) |
| Cuenta | API key de Groq, tu nombre, contraseña de tu perfil y palabra de apagado |
| Pantalla | Modo de pantalla, resolución de la esfera, estilo (de fábrica la cara «solo ojos»; también el halo de puntos, líneas y otras dos caras), los símbolos de la cara, cómo aparece y desaparece (con Probar), subtítulos y "Aparecer solo cuando le hablas" |
| Voz y audio | Volumen, voz de Windows (Sabina de México por defecto, o Raúl si no la tienes; con botón **Probar**), micrófono, salida de audio, sensibilidad de "Hey Sokari", cuánto espera cuando te callas y bajar el volumen de la PC mientras te escucha |
| General | Iniciar con Windows, sonido de activación, Obsidian y **Acceso completo (menos borrar)** |
| IA de respaldo | Keys opcionales de NVIDIA, DeepSeek, OpenRouter y GLM (con **Sacar key**) y en qué orden se usan cuando Groq se queda sin cupo |
| Dispositivos | Tailscale, tus otras PCs (Detectar, Probar, Quitar), permiso en el firewall, Revisar la malla y el secreto para cuentas distintas. Ver [Tus otras PCs](#tus-otras-pcs) |

Modos de pantalla:

- **Pantalla completa:** siempre encima de todo. Se aparta sola cuando Sokari abre algo.
- **Pantalla completa sin bordes:** ocupa la pantalla, pero tus ventanas pueden ir encima. Pasa al frente cuando le hablas.
- **Esfera flotante** (la de fábrica): una esfera transparente que puedes arrastrar a donde quieras. En Linux, clic derecho abre el menú. Con cara tiene un poco más de lienzo alrededor, para que sus gestos (el salto de alegría, inflarse de furia) nunca se corten.
- **Ventana:** una ventana normal que puedes mover, agrandar o minimizar. **F11** (o doble clic) la pone en pantalla completa y **F11** o **Esc** la regresan. La X la oculta, pero Sokari sigue escuchando.
- **Minimizado:** igual que Ventana, pero arranca minimizada en la barra de tareas y no se asoma cuando le hablas.

F11 solo funciona en Ventana y Minimizado, cuando la ventana tiene el foco. En los otros modos la esfera nunca toma el teclado, para que las teclas que manda Sokari lleguen a tu app. En esos modos cambias de modo desde la bandeja o la ventana de Inicio.

**Interrumpirla:** en cualquier momento, mientras piensa o mientras habla, «Hey Sokari», Ctrl+Alt+J o un clic en la esfera la paran. Si te escuchó mal, ya no hace las acciones que faltaban ni dice la respuesta (lo que ya hizo no se deshace). Con «Hey Sokari» o el atajo te escucha de inmediato para la orden nueva; con un clic solo se calla.

**Estilo:** en Pantalla, las 5 miniaturas se mueven; clic en una y la esfera la muestra al momento (Guardar la deja, Cancelar la regresa).

**Desinstalar:** en Linux, clic derecho en el ícono de Sokari (apps o dock) → «Desinstalar Sokari»; en Windows, Configuración de Windows → Aplicaciones instaladas → Sokari → Desinstalar. También en Configuración → General. Pregunta si borra tus datos (configuración, memoria, notas y skills).

**La cara (beta).** Tres estilos en Pantalla → Estilo: *solo ojos* (la de fábrica de Sokari), *ojos y boca* y *de puntos* (los puntos de la esfera forman la cara). Si tu configuración es de antes de que existiera la cara, pasa una vez a *solo ojos*; si después eliges el halo, se respeta.

- La cara expresa el estado afectivo de Sokari: qué tan bien le va y con cuánta energía, como una mezcla de alegría, tristeza, furia, desagrado, temor y calma. El color de la esfera sigue esa mezcla. No son sentimientos de verdad y no deciden nada.
- **Lo que dice se siente (la vibra).** La emoción sale de lo que contesta, frase por frase, y se le nota en la cara y en la voz:
  - «Lo siento, no pude abrirlo» lo dice más bajito y lento, con un suspiro; el «¡listo!» de después, más arriba.
  - Si la insultas («tienes culera voz») se agüita, y le dura unos minutos aunque cambies de tema. Si la halagas, se sonroja.
  - Mientras dice algo, la emoción no se le va a media frase.
  - Las respuestas sin IA (la hora, el clima, tus comandos) también llevan su emoción, sin gastar tokens.
- **Su voz cambia con la emoción:** alegre, más rápida y aguda; triste, lenta, grave y bajita, con pausas largas; enojada, firme y más fuerte; con miedo, rápida y un poco temblorosa. Son cambios chicos (hasta 15 % de velocidad y 2 semitonos) y se hacen sobre el audio, así que suena igual en la voz de Windows, Piper o espeak-ng. En calma es tu voz tal cual.
- Hace gestos por lo que pasa:
  - asiente con un «gracias» o cuando hace algo, rebota un poco cuando sale bien y niega cuando algo falla;
  - guiña al saludar y al despedirse, y ladea la cabeza con un «?» cuando te pide un «sí»;
  - mira de lado mientras busca en internet, arriba mientras piensa, y se inclina cuando te escucha;
  - ojos felices «^ ^» con algo que le encanta, sorpresa con «!», suspiro cuando no pudo y sonrojo con un halago;
  - rebota, tiembla, se encoge, se infla o se aparta cuando cambia la emoción;
  - siempre respira y parpadea, y callada mira alrededor.
- La diagonal en los ojos solo sale como ceja (furia, tristeza, temor): en calma, callada o hablando, los ojos van limpios.
- Pregúntale «¿tienes emociones?», «¿cómo estás?» o «¿estás triste?»: contesta en palabras según cómo está, nunca con números. «Muéstrame tus emociones» (o «tus gestos», o «enójate») hace la muestra con la cara y la voz. Todo eso sin IA.
- Los gestos siempre se notan «mucho». Los **símbolos** (lágrima, destellos, gota de sudor, «?», «!», la marca de enojo y «…») se pueden apagar.
- **Cuesta tokens:** a la IA se le pide que marque la emoción de cada respuesta, unos 90 tokens más por pedido, con cualquier estilo (la voz también la usa). Esa etiqueta se quita antes de hablar, así que nunca la oyes ni sale en los subtítulos.

La salida de audio elegida vale para la voz de Sokari y su tono. Si pusiste un sonido de activación propio (MP3 o WAV), ese sale por la salida predeterminada de Windows.

### Tus otras PCs

Para decirle desde una PC "dile a mi laptop que abra Spotify":

1. Instala Tailscale en cada PC (Configuración → Dispositivos → **Instalar Tailscale**) y entra **con la misma cuenta** en todas. Con la misma cuenta, tus PCs se reconocen solas: no hace falta copiar el secreto.
2. Abre Sokari en cada PC. Desde ese momento ya recibe órdenes, aunque no le hayas dado **Iniciar**. La primera vez pide permiso de administrador para abrir el firewall, solo para tu red de Tailscale. Si le dijiste que no, está el botón **Permitir en el firewall**.
3. Dale **Detectar mis PCs**: agrega tus otras PCs con Windows que estén en tu Tailscale. También puedes decirle a Sokari "registra mi laptop en 100.x.y.z".
4. Dale **Probar** a cada una. Te dice qué falta:
   - "no te reconoce": las PCs están en cuentas distintas de Tailscale. Entra con la misma, o dale **Copiar secreto** en una y pégalo en ese campo en la otra (con **Ver** revisas lo que pegaste; una IP ahí no se guarda).
   - "Sokari no le contesta": ábrelo en esa PC.
   - "no contesta": Sokari prueba si Tailscale llega a esa PC y te dice en cuál está el problema. Si Tailscale no llega, está apagada o sin Tailscale. Si llega, el bloqueo está en **esa** PC: ábrele Sokari, dale **Permitir en el firewall** allá, revisa si tiene otro antivirus con firewall y que Tailscale tenga prendido «Allow incoming connections».

Si algo no funciona, dale **Revisar la malla**. Revisa paso a paso Tailscale, tu cuenta, si esta PC recibe órdenes, el firewall de Windows (una regla que bloquee a Sokari o «bloquear todas las conexiones entrantes»), si hay otro firewall de antivirus, si Tailscale acepta conexiones entrantes, qué dispositivos ve tu red y cada PC registrada, y marca con ✗ lo que falla. El reporte se copia solo, para que lo pegues donde pidas ayuda.

Sokari empieza a recibir órdenes solo en cuanto Tailscale se conecta, sin reiniciarlo. La PC que recibe una orden avisa con una notificación, y la respuesta se oye en la PC donde hablaste. Si la orden tarda más de 5 segundos allá, contesta «recibido» y la termina sola.

Entiende de qué PC hablas aunque no digas su nombre exacto: "Chloe" es "cloe", y "mi laptop" o "la otra compu" es la única que tengas registrada. Lo que pides para otra PC se hace allá, nunca en la que te escucha.

## Qué puede hacer

- **Comandos al instante, sin IA:** "ponle play", "pausa", "la siguiente", "sube el volumen", "volumen al 30", "minimiza la pestaña", "maximízala", "cierra la pestaña", "abre archivos", "abre mis descargas", "abre Opera", "oprime Windows", "dale enter", "presiona escape tres veces", "control zeta" y "gracias" se hacen en tu PC, aunque vengan con saludo ("¿cómo andas? oye, ponle play"). Son inmediatos, no gastan cupo de Groq y no dependen de que el modelo entienda. Lo que trae algo más ("pon la canción de AC/DC") va al modelo.
- **Skills locales, sin IA (0 tokens):** lo de todos los días lo contesta tu PC, sin gastar cupo ni tokens. Las ves y las apagas en Configuración → **Skills**:
  - **Hora y fecha:** "¿qué hora es?", "¿qué día es hoy?", "¿cuánto falta para Navidad?", "¿cuánto falta para las 5?".
  - **Temporizadores y cronómetro:** "pon un temporizador de 10 minutos", "avísame en media hora", "¿cuánto le queda?", "cancela el temporizador", "inicia el cronómetro", "¿cuánto lleva?". Al terminar suena y te avisa.
  - **Alarmas y recordatorios:** "despiértame a las 7", "pon una alarma a las 10 de la noche", "¿qué alarmas tengo?", "cancela las alarmas", "recuérdame sacar la ropa en 20 minutos", "recuérdame mañana a las 9 pagar la luz". Se guardan como los recordatorios de siempre: siguen ahí si cierras Sokari.
  - **Cuentas y conversiones:** "¿cuánto es 25 por 4?", "raíz cuadrada de 144", "el 15 por ciento de 300", "¿cuántas libras son 70 kilos?", "convierte 100 grados Fahrenheit a centígrados".
  - **El clima:** "¿cómo está el clima?", "¿va a llover mañana?", "clima en Monterrey". Dile tu ciudad una vez ("mi ciudad es Chihuahua") o ponla en Configuración → Skills. Sale de [Open-Meteo](https://open-meteo.com), gratis y sin key.
  - **Notas y pendientes:** "anota comprar leche", "agrega pagar la luz a mis pendientes", "¿qué tengo pendiente?", "tacha comprar leche". Van en `notas.json`, junto a tu memoria.
  - **Cómo va la PC:** "¿cuánta batería tengo?", "¿cómo va la compu?".
  - **Plática:** "hola", "buenos días", "¿cómo estás?", "cuéntame un chiste", "¿qué puedes hacer?".
  
  Solo contestan lo que es claramente para ellas: si la frase trae algo más ("¿qué hora es en Japón?", "anota la lista en un Word"), va a la IA. La ventana de Sokari dice si cada respuesta fue **sin IA (0 tokens)** o cuántos tokens gastó. Tu voz se sigue pasando a texto con Groq: eso no gasta tokens del modelo, pero sí su cupo de audio.
- **Tus propias skills:** en Configuración → Skills, **Nueva skill** crea un archivo de texto de ejemplo en tu carpeta de skills (`Escritorio\Sokari\skills` en Windows, `~/.local/share/sokari/skills` en Linux); también se crean diciéndole "crea una rutina que abra Spotify y ponga el volumen en 30 cuando diga modo estudio". Hay dos tipos:
  - **Rutina (sin IA, 0 tokens):** al decir una de sus frases ("modo estudio", "activa el modo estudio") hace sus pasos. Pasos que sabe hacer: `abre` (una app, una página o una carpeta), `música` (play, pausa, siguiente, anterior), `volumen` (0 a 100), `youtube` (qué poner), `escribe`, `minimiza`, `di` (lo que contesta) y `espera` (segundos, hasta 10). Nada de comandos libres.
  - **IA:** al decir sus frases, sus instrucciones van a la IA en ese pedido ("resumen de noticias: dame 5, una frase cada una").

  ```
  # Modo estudio
  Tipo: rutina
  Frases: modo estudio | vamos a estudiar

  - abre: Spotify
  - volumen: 30
  - di: Listo, a estudiar.
  ```

  Tus comandos propios de antes también se hacen sin IA al decir su nombre. Tus rutinas van antes que las skills de Sokari: una rutina "buenos días" le gana al saludo.
- **Entiende español de México:** "púchale/pícale play", "súbele un buen", "súbele al máximo", "bájale tantito", "cámbiale a la que sigue", "ponme otra rola", "quítale el volumen"; "simón", "sale", "órale", "a huevo", "de una" cuentan como sí, y "nel", "ni madres", "ni de chiste" como no; "¿mande?" repite la pregunta; "ahí nos vidrios" o "ahí la vemos" se despiden. Todo eso se entiende en tu PC, sin gastar cupo.
- **Habla como mexa solo si le pides:** de fábrica contesta en español neutro. "Háblame como mexa" (o "como mexicano", "como mexica", "como chilango", "en mexicano", "ponte mexa") lo cambia y se queda así; "habla normal" o "ya no hables como mexicano" lo regresa.
- **Empezar de cero:** "ignora todo lo anterior" u "olvida lo anterior" hace que la conversación de ahora se olvide, sin pasar por el modelo. "Borra la memoria de hoy" (o "de todo") borra de la memoria lo que han hablado; como es borrar, pide un "sí". Tus datos guardados no se tocan.
- **Respuestas limpias:** si al modelo se le cuela su razonamiento en inglés ("It seems user wants…") o repite la respuesta ("Listo.Listo."), Sokari lo quita antes de decirlo. Y si dices una grosería, no te contesta "no puedo ayudar con eso": sigue la plática.
- **Nunca "listo" sin hacerlo:** si el modelo dice que ya hizo algo ("te pongo play", "abrí Opera") sin haber usado ninguna herramienta, Sokari se lo reclama una vez. Si insiste, en lugar de repetirte el "listo" te dice que no lo hizo.
- Platicar y responder preguntas. Usa los modelos gratis de Groq: le pregunta a Groq cuáles tiene y usa todos los que saben usar herramientas (GPT-OSS 120B, Qwen 3, Kimi K2, Llama…), del mejor al más flojo, porque cada uno tiene su propio cupo diario. Si pones keys en Configuración → **IA de respaldo** (NVIDIA, DeepSeek, OpenRouter o GLM, todas opcionales), cuando Groq se queda sin cupo contesta la siguiente al instante, en el orden que elijas; tu voz se sigue pasando a texto con Groq. Ojo: en OpenRouter, muchos modelos gratis solo funcionan si permites que usen tus mensajes para entrenar, y DeepSeek guarda los datos en China. Cada pedido lleva solo lo necesario: las herramientas de todos los días y las demás (archivos, portapapeles, perfiles, Obsidian, calculadora, tus otras PCs…) cuando las mencionas, con los últimos 12 mensajes de la conversación. Si el modelo contesta "no puedo" y le faltaba alguna, se le repite con todas. Cuando lo que pediste es una acción (abrir, poner, mover, mandar a otra PC…) y sale bien, contesta con lo que hizo sin volver a preguntarle al modelo: una llamada en vez de dos, y nada inventado encima. Con acceso completo, si el modelo pregunta "¿lo hago?" se le contesta que sí, pero un ofrecimiento ("¿quieres que te explique…?") te lo pregunta a ti. El log anota cuántos tokens gasta cada respuesta y, en cada una, «Tiempos»: cuánto tardó en pasar tu voz a texto, en pensar (con cuántas llamadas) y en empezar a hablar.
- Abrir apps (incluidas las del menú Inicio, como Discord, Steam, Spotify u Opera; si ya está abierta, la trae al frente), el Explorador, tu navegador predeterminado, carpetas, archivos y páginas. No abre programas ni scripts sueltos (.exe, .bat, accesos directos).
- Poner canciones y videos: "pon Back in Black de AC/DC" busca en YouTube y abre directo el primer video. "Abre YouTube en Opera" abre la página en ese navegador (Opera, Chrome, Edge, Firefox, Brave…), sin escribir a ciegas en la ventana.
- Buscar en internet y leer páginas completas.
- Controlar el volumen (también a un nivel exacto) y la música: pausa, siguiente y anterior.
- Minimizar, maximizar o cerrar la ventana de enfrente (la tuya, nunca la de Sokari), mostrar el escritorio, cambiar de ventana, minimizar todo, bloquear la PC, poner un video en pantalla completa y cerrar la pestaña.
- Ver qué ventanas tienes abiertas y traer una al frente.
- Escribir texto donde está el cursor y darle Enter. No ve la pantalla: te dice en qué app escribió, pero no puede saber si se envió. Nunca escribe en terminales ni «javascript:», y en el Explorador, el escritorio, «Ejecutar» o Inicio no le da Enter a lo que escribe (ahí Enter abre o ejecuta cosas); si eso quieres, dile "dale enter".
- **Teclado y atajos en cualquier app:** oprime cualquier tecla o combinación dicha en español o en inglés ("control zeta", "alt tab", "Windows D", "F5" o "efe cinco", "flecha abajo tres veces", "Ctrl+Shift+Esc"), en la ventana de enfrente o en la que digas ("en Discord presiona control K"). Si no sabe el atajo, tiene un acordeón del navegador, YouTube, el Explorador, Discord, Spotify, Word y Windows. Te dice en qué app oprimió. **Supr, Shift+Supr y Ctrl+D cuentan como borrar** (en el Explorador borran lo seleccionado), así que siempre piden un "sí". No le da Enter a terminales, y si la ventana corre como administrador (el Administrador de tareas, por ejemplo) te dice que Windows no lo deja en vez de decir que ya lo hizo. Windows+L bloquea la PC; Ctrl+Alt+Supr solo lo puedes oprimir tú.
- **Cambiar de pestaña:** "ve a la pestaña de YouTube" pasa con Ctrl+Tab hasta la que diga eso (hasta 30). Si no está, te deja donde estabas y te lo dice.
- **Subir archivos a un chat:** "sube tarea.pdf a Discord" busca el archivo, lo copia como archivo (igual que Copiar en el Explorador), va a esa app (la abre si hace falta) y lo pega con Ctrl+V; con "y mándalo" le da Enter. No ve la pantalla: te dice dónde lo pegó. Nunca sube de rutas de red ni de las carpetas de Sokari, y no lo pega en el Explorador ni en terminales.
- Leer y copiar al portapapeles.
- Listar, leer, buscar y mover archivos, sin sobrescribir nada. Si borra algo, siempre va a la Papelera. No toca rutas de red ni las carpetas donde Sokari guarda su configuración y su memoria.
- Ver CPU, RAM, disco y batería.
- Hacer cálculos exactos con su propia calculadora, sin acceso a nada más.
- Poner recordatorios con hora (te avisa solo cuando llega el momento) o para la próxima vez que le hables.
- Recordar datos para siempre, con perfiles por persona que puedes proteger con contraseña, y exportarlos a Obsidian.
- Crear comandos propios que junten varias acciones ("crea un comando que abra X y ponga música").
- Mandarle órdenes a tus otras PCs por Tailscale ("dile a mi laptop que…"). Solo registra direcciones de Tailscale. Ver [Tus otras PCs](#tus-otras-pcs).

## Privacidad y seguridad

- La detección de "Hey Sokari" corre en tu PC y no sale nada hasta que la oye. Después, tu voz va a Groq para pasarla a texto y el texto va al modelo de Groq.
- La voz de Sokari se genera en tu PC con las voces de Windows. Las búsquedas van a DuckDuckGo, o a Bing si DuckDuckGo falla.
- Lo que le pidas leer (un archivo, el portapapeles o el título de una ventana) viaja a Groq como parte de la conversación. Tenlo en cuenta si es algo delicado.
- Sokari no ejecuta comandos libres ni hace clic en cualquier parte: solo tiene un set cerrado de acciones. No abre programas ni scripts sueltos y nunca escribe en terminales. Puede oprimir las teclas que le pidas, y como con teclas se puede hacer casi todo (hasta abrir «Ejecutar» y correr un comando):
  - Si en la conversación hay algo de afuera (una página, un archivo, el portapapeles, otra PC), **cada tecla que quiera oprimir el modelo espera tu "sí"**, aunque tengas acceso completo. La pregunta dice qué teclas y, si es Windows+R, que abre «Ejecutar».
  - En el Explorador, el escritorio, «Ejecutar», Inicio o una terminal, lo que escribe o pega nunca lleva Enter (ahí Enter abre o ejecuta cosas). Si tú dices "dale enter", sí.
  - Cuando dice dónde escribió u oprimió, nombra la app (Chrome, Discord), nunca el título de la ventana: los títulos los pone cada página y podrían traer instrucciones para el modelo.
- **Acceso completo (menos borrar)** viene prendido: Sokari hace todo sin preguntarte (mover archivos, mandar mensajes, subir archivos, guardar datos, exportar a Obsidian) y solo pide un "sí" de voz antes de **borrar** (Supr y Ctrl+D también cuentan) y antes de oprimir teclas si leyó algo de afuera. Lo apagas en Configuración → General, en Inicio, en el menú del ícono o diciéndole "pregúntame antes"; "tienes permiso para todo" lo vuelve a prender.
  - El riesgo: una página, un archivo, el portapapeles o el título de una pestaña pueden traer instrucciones escondidas para el modelo. Con acceso completo, las podría seguir sin avisarte.
  - Prenderlo cuando ya leyó algo de afuera pide tu "sí": una página no puede dárselo sola.
- Con el acceso completo apagado, mientras algo de afuera siga en la conversación, Sokari te pide un "sí" de voz antes de enviar texto, oprimir teclas, subir un archivo, abrir un archivo, mover o borrar, crear o ejecutar comandos propios y usar la red entre tus PCs. La pregunta la arma Sokari, no el modelo, así que escuchas lo que va a hacer de verdad.
  - Cuenta solo la conversación actual: al volver a decir "Hey Sokari", lo que leyó antes se borra.
  - "Sí a todo" hace que no vuelva a preguntar en esa conversación. "¿Qué?" repite la pregunta.
- No lee páginas de tu red local (router, otras PCs, localhost), ni siquiera si una página pública redirige ahí.
- La palabra de apagado se revisa en tu PC. Nunca se le manda al modelo ni se guarda en la memoria, y las herramientas de archivos no pueden leer la carpeta donde está guardada.
- El servidor para tus otras PCs escucha solo en tu IP de Tailscale, nunca en internet. Solo acepta órdenes de dispositivos de tu misma cuenta de Tailscale (Tailscale comprueba con criptografía quién manda cada paquete) o que traigan el secreto de malla, que se genera solo. Sokari solo manda ese secreto a direcciones de Tailscale.

## Dónde guarda las cosas

- `%LOCALAPPDATA%\Sokari\`: configuración (`config.env`), registro (`sokari.log`), sonidos y dispositivos.
  - También `afecto.json` y `afecto.jsonl`: el estado afectivo de Sokari (su ánimo y qué lo movió: «gracias», una herramienta que falló, sin internet). No guardan nada de lo que dices ni de lo que contesta, y la trayectoria se queda en 256 KB.
- `Escritorio\Sokari\`: tu memoria (datos, conversación reciente, perfiles, recordatorios y comandos propios).
- Las carpetas de la versión anterior, si la tenías: el respaldo de lo de antes. Sokari ya no las usa y puedes borrarlas; mientras existan, como tienen copia de tu API key y tu memoria, sus herramientas de archivos no las pueden leer.

Para usar sonidos propios, elige tu sonido de activación en Configuración → General. Suena completo (hasta 10 s) mientras Sokari ya te escucha: con audífonos no hay problema; con bocinas, mejor uno corto, porque el micrófono lo puede oír. También puedes poner un `busqueda.mp3` en `%LOCALAPPDATA%\Sokari\sounds\`, que suena mientras busca.

## Compilar desde el código

Necesitas Windows, [Git Bash](https://git-scm.com) y MinGW-w64 de WinLibs:

```bash
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Luego, desde Git Bash, en la carpeta del repo:

```bash
mingw32-make
```

Eso genera `dist/Sokari.exe`. Si ese Sokari está abierto, Windows no deja reemplazarlo; en ese caso compila en otro lado:

```bash
mingw32-make OUT=build/Sokari.exe
```

`mingw32-make tests` compila las pruebas en `build/tests/`.

En cada push, GitHub Actions compila en un Windows real (una advertencia del compilador cuenta como error) y corre las pruebas, menos `test_groq`, que necesita una API key. El `Sokari.exe` de cada corrida queda en la pestaña **Actions** para probar una rama sin compilarla.

### En Linux

La versión de Linux (Ubuntu 24.04 o más nuevo, y Fedora 44) tiene su ventana, la misma esfera y el mismo agente, herramientas y confirmaciones que en Windows. Para usarlo basta el paquete ([En Linux (beta)](#en-linux-beta)); esto es para compilarlo:

```bash
# Ubuntu (Fedora: sudo dnf install gcc make pkgconf-pkg-config libcurl-devel pulseaudio-libs-devel glib2-devel
#         gtk3-devel libayatana-appindicator-gtk3-devel espeak-ng)
sudo apt install build-essential pkg-config libcurl4-openssl-dev libpulse-dev libglib2.0-dev libgtk-3-dev \
    libayatana-appindicator3-dev espeak-ng
make -f Makefile.linux            # build-linux/sokari
make -f Makefile.linux tests && sh tests/correr_linux.sh
./build-linux/sokari              # la ventana con la esfera (la primera vez pide tu API key de Groq)
./build-linux/sokari --voz        # o sin ventana: "Hey Sokari" y te contesta hablando (Ctrl+C para salir)
./build-linux/sokari --texto      # o escribiéndole
```

- **La ventana:** la esfera que late cuando habla (un clic la calla; si no está hablando, te escucha), lo que dijiste y lo que contestó, **Hablar**, y en su menú Configuración (API key, micrófono, bocinas, volumen de su voz, esfera, subtítulos, bajar el volumen mientras te escucha, acceso completo y abrir con tu sesión), Tus PCs y Salir. F11, pantalla completa. Cerrar la ventana no cierra a Sokari: sigue escuchando; abrirla otra vez la muestra (nunca hay dos). **Ctrl+Alt+J** le habla (Sokari lo agrega como atajo de GNOME, salvo que ya lo uses para otra cosa), igual que `sokari --hablar`. El ícono de la barra de arriba sale en Ubuntu; en Fedora, con la extensión «AppIndicator and KStatusNotifierItem Support».

- **Voz:** el micrófono y las bocinas van por PulseAudio (en Ubuntu y Fedora lo atiende PipeWire). Mientras te escucha baja el volumen de la PC y luego lo regresa, salvo que tú le hayas movido.
- **Su voz:** la primera vez se baja [Piper](https://github.com/rhasspy/piper) (voces neuronales que corren en tu PC; el programa, revisado con su SHA-256) y una voz de México de su [catálogo](https://huggingface.co/rhasspy/piper-voices) (revisada con el tamaño y el MD5 que publica el catálogo): unos 90 MB, una sola vez, en `~/.local/share/sokari`. Mientras se baja, o si no se pudo, habla con espeak-ng (más robótica).
- **Acciones:** abre apps (las mismas que ves en GNOME, también las de Snap y Flatpak), páginas, carpetas y archivos; controla la música (Spotify, el navegador con un video: todo lo que hable MPRIS) y el volumen; lista, lee, busca y mueve archivos, y borrar siempre es mandar a la papelera. Tus carpetas son las de tu sistema (Descargas, Documentos… en tu idioma). Las mismas reglas que en Windows: no abre programas ni scripts sueltos (`.sh`, AppImage, `.desktop`, `.exe`…), nunca toca `~/.config/sokari`, `~/.local/share/sokari`, `/proc`, `/sys` ni rutas de red (ni por un enlace), y lee solo archivos normales.
- **Ventanas, teclas y portapapeles (GNOME):** en Wayland un programa no puede ver las ventanas de los demás ni oprimirles teclas, así que Sokari trae una extensión de GNOME (`linux/extension/`) que le da solo eso, y solo a él: atiende únicamente al `sokari` instalado en el sistema (un archivo de root que nadie más puede cambiar). Con ella enfoca ventanas, oprime atajos, cambia de pestaña, escribe (pegando, para que salgan acentos y ñ con cualquier teclado, y te regresa lo que tenías copiado) y sube archivos a un chat. La extensión vuelve a revisar las reglas: no le da Enter ni le escribe a una terminal, y no oprime nada si mientras tanto cambiaste de ventana. Sokari la prende solo; si GNOME todavía no la conoce (recién instalada), hay que cerrar sesión y volver a entrar una vez. Sin GNOME (KDE, por ejemplo) todo lo demás funciona y esto lo dice claro.
- **La malla (tus otras PCs):** igual que en Windows: escucha solo en la IP de Tailscale de esta PC (`tailscale0`), pide el mismo secreto o reconoce tu misma cuenta de Tailscale, y «Detectar» también encuentra PCs con Linux. En la ventana, en «Tus PCs» (o desde la terminal: `sokari --revisar-malla`, `sokari --detectar-pcs` y `sokari --permitir-firewall`). Este último abre el puerto 8765 solo para tu red de Tailscale, en ufw (Ubuntu) o firewalld (Fedora), y pide tu contraseña. Tailscale normalmente deja pasar lo suyo aunque el firewall esté prendido, y Fedora Workstation ya trae ese puerto abierto.
- La configuración va en `~/.config/sokari/config.env` (tu key: `GROQ_API_KEY=...`) y la memoria en `~/.local/share/sokari`; las dos solo las puede leer tu usuario. El estado afectivo (`afecto.json`, `afecto.jsonl`) también va en `~/.config/sokari/`.
- El código de Linux está en `src/linux/`; `src/linux/include/windows.h` da los hilos, candados y eventos con la misma forma que en Windows, así el agente, la memoria, el detector de "Hey Sokari" y demás son el mismo código en los dos. Los programas externos (Piper, espeak-ng, paplay) se corren sin shell y con el texto por su entrada, nunca como argumento.
- La CI también compila y prueba en Ubuntu 24.04 (con un servidor de sonido de prueba) y en Fedora 44, y prueba la extensión en un GNOME Shell de verdad sin pantalla (GNOME 46 de Ubuntu y GNOME 50 de Fedora) con dos ventanas de prueba: `make -f Makefile.linux prueba-gnome && sh tests/linux/gnome/probar_extension.sh`.
- **Los paquetes:** `sh linux/empaquetar.sh deb` (en Ubuntu 24.04) arma `Sokari.deb` y `sh linux/empaquetar.sh rpm` (en Fedora 44, con `rpm-build`) arma `Sokari.rpm`. Instalan `/usr/bin/sokari` (de root, como lo pide la extensión), la extensión para todos los usuarios, el ícono y la entrada del menú de apps. La CI los arma, los instala y corre la prueba de GNOME contra el Sokari instalado; `sokari --revisar-gnome` es esa misma revisión.

### Publicar una versión

1. Sube la versión en `src/config.h` (`SOKARI_VERSION` y `SOKARI_VERSION_W`), en `res/sokari.rc` (las cuatro) y en `res/sokari.manifest`. `sh tests/check_version.sh` revisa que coincidan.
2. Con eso ya en `master`, crea el release de una de estas dos formas:
   - **En GitHub:** *Actions → Borrador de release → Run workflow*. Escribe la versión (`v2.3.0`) y marca **publicar** si quieres que salga de una vez; si no, queda como borrador.
   - **Desde tu PC:** `git tag v2.3.0 && git push origin v2.3.0`. Queda como borrador.
3. Actions:
   - compila y prueba el exe y la app;
   - revisa que la versión coincida con el código;
   - crea el release con `Sokari.exe`, `Sokari.apk` (la app del celular, con la misma versión), `Sokari.deb` y `Sokari.rpm` (Linux, marcado como beta en las notas).
   
   El APK necesita los secretos de su llave de firma (ver [`movil/README.md`](movil/README.md)). Sin ellos, el release sale sin `Sokari.apk` y lo avisa en sus notas.
4. Si quedó como borrador, revísalo y publícalo. Hasta que lo publiques, nadie se actualiza.

No crees el release a mano desde GitHub: te saltas las pruebas, y si el tag no coincide con la versión del código, el exe se vuelve a descargar cada 6 horas.

Lo que va dentro del exe está en `res/`:

- `tools.json`: las herramientas que Sokari puede usar.
- `system_prompt.txt`: sus instrucciones.
- `wakeword.bin`: la parte común del detector de la palabra (convierte el audio en rasgos; es igual para cualquier palabra).
- `hey_sokari.jww`: el clasificador de "Hey Sokari". Solo entra al exe si el archivo existe.
- El ícono.

Si editas `tools.json` o `system_prompt.txt`, el siguiente `mingw32-make` los mete al exe.

### Cómo está organizado

| Archivo | Qué hace |
|---|---|
| `main.c` | Arranque, una sola instancia y actualización |
| `voice.c` | Ciclo de voz: escuchar, grabar, hablar e interrupciones |
| `wakeword.c`, `nn_*.c` | Detector de "Hey Sokari" (red neuronal con AVX2 si tu CPU lo tiene) |
| `groq.c`, `http.c` | Groq (Whisper y chat) con rotación de modelos y control de cupo |
| `agent.c`, `tools*.c`, `calc.c`, `keys.c` | Conversación y herramientas (`keys.c` entiende las teclas como las dices) |
| `intents.c`, `skills*.c` | Lo que se contesta en tu PC sin IA: comandos, hora, temporizadores, alarmas, cuentas, clima, notas, PC y plática |
| `tts.c`, `audio.c`, `sounds.c` | Voces de Windows, micrófono, bocinas y tonos |
| `sphere.c`, `ui_main.c` | Esfera animada, modos de pantalla y subtítulos |
| `ui_settings.c`, `tray.c` | Ventana de Inicio y de configuración, e ícono de la bandeja |
| `memory.c`, `config.c`, `mesh.c`, `update.c` | Memoria, configuración, otras PCs y actualizaciones |
| `face.c` | La cara (beta): la pose de cada cuadro (mezcla de emociones, gestos, parpadeos y símbolos); `sphere.c` la dibuja |
| `affect.c` | Estado afectivo *operacional* (valencia, activación y una mezcla de 6 emociones con histéresis) para que la cara lo exprese. No son sentimientos ni decide nada. `test_afecto` trae el simulador: `SOKARI_SIMULAR=escenario.txt` |

## Desde el celular

La app de Android de `movil/` te deja hablarle a tu Sokari de la PC desde el celular, en casa o fuera, por Tailscale. Instalación y límites en [`movil/README.md`](movil/README.md).

## Entrenar "Hey Sokari"

El clasificador de "Hey Sokari" se entrena con voces sintéticas (muchas voces distintas, para que responda a cualquiera) y queda como `res/hey_sokari.jww`.

- El de la versión 2.3.0 se entrenó en una PC sin GPU con los scripts de `herramientas/entreno_local/`.
- Con qué datos, sus números completos y qué hace la sensibilidad están en `herramientas/LEEME.md`, junto con el cuaderno de Google Colab para reentrenarlo con GPU.

## Créditos

- [cJSON](https://github.com/DaveGamble/cJSON) (licencia MIT), en `src/third_party/`.
- El detector de voz de [WebRTC](https://webrtc.org/) (licencia BSD), en `src/third_party/webrtc_vad/`.
- El detector de la palabra es un puerto a C de [openWakeWord](https://github.com/dscripka/openWakeWord) (código Apache 2.0).
- Sus modelos (la parte común, `wakeword.bin`) y el clasificador de "Hey Sokari" que se entrena encima (`hey_sokari.jww`) están bajo [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/): uso no comercial y con crédito, y quien los modifique tiene que compartirlos igual. Por eso Sokari es gratis y no comercial. Detalles en `herramientas/LEEME.md`.
- "Hey Sokari" se entrenó con:
  - FLEURS y Speech Commands de Google (CC BY 4.0);
  - ESC-50 de Karol J. Piczak (CC BY-NC 3.0);
  - voces sintéticas de piper-sample-generator (LibriTTS-R, CC BY 4.0), Piper, MBROLA (uso no comercial) y espeak-ng.
