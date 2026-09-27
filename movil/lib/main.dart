// Sokari Remoto: le hablas al celular y tu Sokari de la PC hace el trabajo.
// El celular pasa tu voz a texto, se lo manda a la PC por Tailscale y lee en
// voz alta lo que contesta.
import 'dart:async';
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'ajustes.dart';
import 'fallas.dart';
import 'malla.dart';
import 'pantalla_ajustes.dart';
import 'voz.dart';

void main() {
  // Ningún error se queda callado: se ve en pantalla (ver fallas.dart).
  runZonedGuarded(() {
    WidgetsFlutterBinding.ensureInitialized();
    FlutterError.onError = (d) {
      FlutterError.presentError(d);
      Fallas.registrar(d.exception, d.stack);
    };
    PlatformDispatcher.instance.onError = (e, s) {
      Fallas.registrar(e, s);
      return true;
    };
    ErrorWidget.builder = (d) => Material(
          color: fondo,
          child: Center(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Text('Algo falló al dibujar esta parte:\n${d.exception}',
                  style: const TextStyle(color: Color(0xFFFCA5A5))),
            ),
          ),
        );
    runApp(SokariApp(almacen: AlmacenSeguro(), oido: OidoAndroid(), voz: VozAndroid(), arranque: ArranqueAndroid()));
  }, (e, s) => Fallas.registrar(e, s));
}

typedef FabricaCliente = ClienteMalla Function(Ajustes a);

ClienteMalla clienteReal(Ajustes a) => ClienteMalla(direccion: a.direccion, secreto: a.secreto);

const morado = Color(0xFF8B5CF6);
const rosa = Color(0xFFEC4899);
const fondo = Color(0xFF14121B);

class SokariApp extends StatelessWidget {
  const SokariApp({
    super.key,
    required this.almacen,
    required this.oido,
    required this.voz,
    this.crearCliente = clienteReal,
    this.arranque,
  });

  final AlmacenAjustes almacen;
  final Oido oido;
  final Voz voz;
  final FabricaCliente crearCliente;
  final Arranque? arranque;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Sokari',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: morado, brightness: Brightness.dark, surface: fondo),
        scaffoldBackgroundColor: fondo,
      ),
      locale: const Locale('es', 'MX'),
      supportedLocales: const [Locale('es', 'MX'), Locale('es')],
      localizationsDelegates: GlobalMaterialLocalizations.delegates,
      home: PantallaPrincipal(almacen: almacen, oido: oido, voz: voz, crearCliente: crearCliente, arranque: arranque),
    );
  }
}

enum Estado { inactivo, escuchando, enviando, hablando }

class Mensaje {
  const Mensaje(this.texto, {this.deTi = false, this.error = false});

  final String texto;
  final bool deTi;
  final bool error;
}

class PantallaPrincipal extends StatefulWidget {
  const PantallaPrincipal({
    super.key,
    required this.almacen,
    required this.oido,
    required this.voz,
    required this.crearCliente,
    this.arranque,
  });

  final AlmacenAjustes almacen;
  final Oido oido;
  final Voz voz;
  final FabricaCliente crearCliente;
  final Arranque? arranque;

  @override
  State<PantallaPrincipal> createState() => _PantallaPrincipalState();
}

class _PantallaPrincipalState extends State<PantallaPrincipal> {
  Ajustes? _ajustes;
  ClienteMalla? _cliente;
  Estado _estado = Estado.inactivo;
  String _parcial = '';
  final _mensajes = <Mensaje>[];
  final _texto = TextEditingController();
  final _scroll = ScrollController();

  /// Los ajustes cifrados tardan en abrir (en vez de solo el círculo girando).
  bool _lento = false;

  /// Le diste «Configurar de nuevo» mientras tardaban: lo que llegue tarde ya no cuenta.
  bool _deNuevo = false;

  @override
  void initState() {
    super.initState();
    _cargar();
    _revisarArranque();
  }

  @override
  void dispose() {
    _cliente?.cerrar();
    _texto.dispose();
    _scroll.dispose();
    super.dispose();
  }

  Future<void> _cargar() async {
    final aviso = Timer(const Duration(seconds: 3), () {
      if (mounted && _ajustes == null) setState(() => _lento = true);
    });
    try {
      final a = await widget.almacen.cargar();
      if (!mounted || _deNuevo) return;
      _usar(a);
      if (a.completos && a.escucharAlAbrir) _hablar();
    } catch (e, s) {
      Fallas.registrar(e, s, 'No pude leer tus ajustes guardados');
      if (mounted && _ajustes == null) _usar(const Ajustes());
    } finally {
      aviso.cancel();
    }
  }

  /// Si la vez pasada Android no alcanzó a mostrar la app (o se cerró por un
  /// error), aquí se ve qué pasó.
  Future<void> _revisarArranque() async {
    final e = await widget.arranque?.ultimoError();
    if (e == null || e.trim().isEmpty || !mounted) return;
    Fallas.ultimo.value = 'La vez pasada Sokari no abrió bien. Ya se ajustó sola; si vuelve a pasar, copia esto y '
        'mándalo a quien te ayuda.\n\n$e';
  }

  void _configurarDeNuevo() {
    _deNuevo = true;
    _usar(const Ajustes());
    _abrirAjustes();
  }

  void _usar(Ajustes a) {
    _cliente?.cerrar();
    setState(() {
      _ajustes = a;
      _cliente = a.completos ? widget.crearCliente(a) : null;
    });
  }

  Future<void> _abrirAjustes() async {
    final a = _ajustes;
    if (a == null) return;
    if (_estado == Estado.escuchando) await widget.oido.parar();
    if (_estado == Estado.hablando) await widget.voz.callar();
    if (!mounted) return;
    setState(() => _estado = Estado.inactivo);
    final nuevo = await Navigator.of(context).push<Ajustes>(MaterialPageRoute(
      builder: (_) => PantallaAjustes(ajustes: a, almacen: widget.almacen, crearCliente: widget.crearCliente),
    ));
    if (nuevo != null && mounted) _usar(nuevo);
  }

  /// El botón grande: escuchar, dejar de escuchar o callar a Sokari.
  Future<void> _hablar() async {
    final a = _ajustes;
    if (a == null) return;
    if (!a.completos) return _abrirAjustes();
    switch (_estado) {
      case Estado.hablando:
        await widget.voz.callar();
        if (mounted) setState(() => _estado = Estado.inactivo);
        return;
      case Estado.escuchando:
        await widget.oido.parar();
        return;
      case Estado.enviando:
        return;
      case Estado.inactivo:
        break;
    }
    if (!await widget.oido.preparar()) {
      _avisar(mensajeErrorOido('no_disponible'));
      return;
    }
    if (!mounted) return;
    setState(() {
      _estado = Estado.escuchando;
      _parcial = '';
    });
    await widget.oido.escuchar(
      alOir: (texto, esFinal) {
        if (!mounted || _estado != Estado.escuchando) return;
        setState(() => _parcial = texto);
        if (esFinal) _enviar(texto);
      },
      alFallar: (error) {
        if (!mounted) return;
        setState(() {
          _estado = Estado.inactivo;
          _parcial = '';
        });
        _avisar(mensajeErrorOido(error));
      },
      alTerminar: () {
        if (!mounted || _estado != Estado.escuchando) return;
        final dicho = _parcial;
        if (dicho.trim().isNotEmpty) {
          _enviar(dicho);
        } else {
          setState(() => _estado = Estado.inactivo);
        }
      },
    );
  }

  Future<void> _enviar(String texto) async {
    final t = texto.trim();
    final a = _ajustes;
    final cliente = _cliente;
    if (a == null || cliente == null) return;
    if (t.isEmpty) {
      setState(() {
        _estado = Estado.inactivo;
        _parcial = '';
      });
      _avisar(mensajeErrorOido('error_no_match'));
      return;
    }
    setState(() {
      _mensajes.add(Mensaje(t, deTi: true));
      _estado = Estado.enviando;
      _parcial = '';
    });
    _bajar();
    try {
      final respuesta = await cliente.enviar(t);
      if (!mounted) return;
      setState(() {
        _mensajes.add(Mensaje(respuesta));
        _estado = a.leerRespuestas ? Estado.hablando : Estado.inactivo;
      });
      _bajar();
      if (a.leerRespuestas) {
        await widget.voz.decir(respuesta);
        if (mounted && _estado == Estado.hablando) setState(() => _estado = Estado.inactivo);
      }
    } on ErrorMalla catch (e) {
      if (!mounted) return;
      setState(() {
        _mensajes.add(Mensaje(e.mensaje, error: true));
        _estado = Estado.inactivo;
      });
      _bajar();
    }
  }

  void _escrito() {
    final t = _texto.text;
    if (t.trim().isEmpty || _estado != Estado.inactivo) return;
    _texto.clear();
    FocusScope.of(context).unfocus();
    _enviar(t);
  }

  void _avisar(String mensaje) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(content: Text(mensaje)));
  }

  void _bajar() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) {
        _scroll.animateTo(_scroll.position.maxScrollExtent,
            duration: const Duration(milliseconds: 250), curve: Curves.easeOut);
      }
    });
  }

  String get _estadoTexto => switch (_estado) {
        Estado.inactivo => 'Toca para hablar',
        Estado.escuchando => _parcial.isEmpty ? 'Te escucho…' : _parcial,
        Estado.enviando => 'Preguntándole a tu PC…',
        Estado.hablando => 'Toca para callarlo',
      };

  @override
  Widget build(BuildContext context) {
    final a = _ajustes;
    return Scaffold(
      appBar: AppBar(
        backgroundColor: fondo,
        title: const Text('Sokari', style: TextStyle(fontWeight: FontWeight.w700)),
        actions: [
          IconButton(
            tooltip: 'Configuración',
            icon: const Icon(Icons.settings_outlined),
            onPressed: a == null ? null : _abrirAjustes,
          ),
        ],
      ),
      body: SafeArea(
        child: Column(
          children: [
            ValueListenableBuilder<String?>(
              valueListenable: Fallas.ultimo,
              builder: (_, falla, _) => falla == null ? const SizedBox.shrink() : _falla(falla),
            ),
            Expanded(
              child: a == null
                  ? _abriendo()
                  : Column(
                      children: [
                        Expanded(child: a.completos ? _conversacion() : _sinConfigurar()),
                        if (a.completos) _controles(),
                      ],
                    ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _abriendo() {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const CircularProgressIndicator(),
            if (_lento) ...[
              const SizedBox(height: 20),
              const Text(
                'Tus ajustes guardados están tardando en abrir.',
                key: Key('lento'),
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 8),
              TextButton(
                key: const Key('configurar_de_nuevo'),
                onPressed: _configurarDeNuevo,
                child: const Text('Configurar de nuevo'),
              ),
            ],
          ],
        ),
      ),
    );
  }

  /// Un error, arriba y a la vista, con «Copiar» para mandárselo a quien te ayuda.
  Widget _falla(String texto) {
    return Material(
      key: const Key('falla'),
      color: const Color(0xFF3B1D24),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 8, 2),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('Algo falló', style: TextStyle(fontWeight: FontWeight.w700, color: Color(0xFFFCA5A5))),
            const SizedBox(height: 4),
            Text(texto, maxLines: 4, overflow: TextOverflow.ellipsis, style: const TextStyle(color: Colors.white70)),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  key: const Key('copiar_falla'),
                  onPressed: () async {
                    await Clipboard.setData(ClipboardData(text: texto));
                    _avisar('Copiado');
                  },
                  child: const Text('Copiar'),
                ),
                TextButton(
                  key: const Key('cerrar_falla'),
                  onPressed: () => Fallas.ultimo.value = null,
                  child: const Text('Cerrar'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _sinConfigurar() {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Icon(Icons.computer, size: 56, color: morado),
            const SizedBox(height: 16),
            const Text('Conecta tu PC', style: TextStyle(fontSize: 22, fontWeight: FontWeight.w700)),
            const SizedBox(height: 8),
            const Text(
              'Sokari vive en tu PC. Desde aquí le hablas a través de Tailscale: pon la dirección de tu PC y su '
              'secreto de malla.',
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 20),
            FilledButton(key: const Key('configurar'), onPressed: _abrirAjustes, child: const Text('Configurar')),
          ],
        ),
      ),
    );
  }

  Widget _conversacion() {
    if (_mensajes.isEmpty) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(32),
          child: Text(
            'Pídele lo que sea a tu PC: “abre Spotify”, “¿qué clima hace mañana?”, “recuérdame a las 6 sacar la '
            'ropa”.',
            textAlign: TextAlign.center,
            style: TextStyle(color: Colors.white70),
          ),
        ),
      );
    }
    return ListView.builder(
      controller: _scroll,
      padding: const EdgeInsets.fromLTRB(12, 12, 12, 4),
      itemCount: _mensajes.length,
      itemBuilder: (_, i) => _Burbuja(_mensajes[i]),
    );
  }

  Widget _controles() {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 12),
      child: Column(
        children: [
          Text(
            _estadoTexto,
            key: const Key('estado'),
            textAlign: TextAlign.center,
            maxLines: 3,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(color: Colors.white70),
          ),
          const SizedBox(height: 12),
          _BotonHablar(estado: _estado, onTap: _hablar),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: TextField(
                  key: const Key('texto'),
                  controller: _texto,
                  enabled: _estado == Estado.inactivo,
                  textInputAction: TextInputAction.send,
                  onSubmitted: (_) => _escrito(),
                  decoration: const InputDecoration(
                    hintText: 'O escríbelo aquí',
                    isDense: true,
                    border: OutlineInputBorder(),
                  ),
                ),
              ),
              IconButton(
                key: const Key('enviar'),
                tooltip: 'Enviar',
                icon: const Icon(Icons.send),
                onPressed: _estado == Estado.inactivo ? _escrito : null,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _BotonHablar extends StatelessWidget {
  const _BotonHablar({required this.estado, required this.onTap});

  final Estado estado;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final icono = switch (estado) {
      Estado.inactivo => const Icon(Icons.mic, size: 40, color: Colors.white),
      Estado.escuchando => const Icon(Icons.stop_rounded, size: 40, color: Colors.white),
      Estado.enviando => const SizedBox(
          width: 32, height: 32, child: CircularProgressIndicator(strokeWidth: 3, color: Colors.white)),
      Estado.hablando => const Icon(Icons.volume_off, size: 36, color: Colors.white),
    };
    return Semantics(
      button: true,
      label: estado == Estado.inactivo ? 'Hablar con Sokari' : 'Detener',
      child: GestureDetector(
        key: const Key('hablar'),
        onTap: onTap,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 200),
          width: estado == Estado.escuchando ? 96 : 84,
          height: estado == Estado.escuchando ? 96 : 84,
          decoration: const BoxDecoration(
            shape: BoxShape.circle,
            gradient: LinearGradient(colors: [morado, rosa], begin: Alignment.topLeft, end: Alignment.bottomRight),
          ),
          child: Center(child: icono),
        ),
      ),
    );
  }
}

class _Burbuja extends StatelessWidget {
  const _Burbuja(this.m);

  final Mensaje m;

  @override
  Widget build(BuildContext context) {
    final color = m.error
        ? const Color(0xFF3B1D24)
        : m.deTi
            ? const Color(0xFF3A2D5C)
            : const Color(0xFF221F2E);
    return Align(
      alignment: m.deTi ? Alignment.centerRight : Alignment.centerLeft,
      child: Container(
        margin: const EdgeInsets.symmetric(vertical: 4),
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
        constraints: BoxConstraints(maxWidth: MediaQuery.sizeOf(context).width * 0.8),
        decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(16)),
        child: SelectableText(m.texto, style: TextStyle(color: m.error ? const Color(0xFFFCA5A5) : Colors.white)),
      ),
    );
  }
}
