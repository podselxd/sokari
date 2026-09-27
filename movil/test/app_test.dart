import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:sokari_remoto/ajustes.dart';
import 'package:sokari_remoto/fallas.dart';
import 'package:sokari_remoto/main.dart';
import 'package:sokari_remoto/malla.dart';
import 'package:sokari_remoto/voz.dart';

class AlmacenFalso implements AlmacenAjustes {
  AlmacenFalso(this.a);

  Ajustes a;
  int guardados = 0;

  @override
  Future<Ajustes> cargar() async => a;

  @override
  Future<void> guardar(Ajustes nuevo) async {
    a = nuevo;
    guardados++;
  }
}

/// Un almacén cifrado que nunca termina de abrir (como un Keystore trabado).
class AlmacenLento implements AlmacenAjustes {
  final nunca = Completer<Ajustes>();

  @override
  Future<Ajustes> cargar() => nunca.future;

  @override
  Future<void> guardar(Ajustes a) async {}
}

/// Uno que truena al leer.
class AlmacenRoto implements AlmacenAjustes {
  @override
  Future<Ajustes> cargar() async => throw Exception('javax.crypto.AEADBadTagException');

  @override
  Future<void> guardar(Ajustes a) async => throw Exception('Keystore no disponible');
}

class ArranqueFalso implements Arranque {
  ArranqueFalso(this.error);

  final String? error;

  @override
  Future<String?> ultimoError() async => error;
}

/// Un micrófono de mentira: "oye" [frase] (primero a medias, luego completa).
class OidoFalso implements Oido {
  String? frase = 'abre Spotify';
  String? error;
  bool disponible = true;
  int escuchas = 0;

  @override
  Future<bool> preparar() async => disponible;

  @override
  Future<void> escuchar({
    required void Function(String texto, bool esFinal) alOir,
    required void Function(String error) alFallar,
    required void Function() alTerminar,
  }) async {
    escuchas++;
    final e = error, f = frase;
    if (e != null) return alFallar(e);
    if (f == null) return alTerminar();
    alOir(f.substring(0, f.length ~/ 2), false);
    alOir(f, true);
  }

  @override
  Future<void> parar() async {}
}

class VozFalsa implements Voz {
  final dichos = <String>[];

  @override
  Future<void> decir(String texto) async => dichos.add(texto);

  @override
  Future<void> callar() async {}
}

const configurada = Ajustes(direccion: '100.64.0.7', secreto: 'secreto');

void main() {
  late OidoFalso oido;
  late VozFalsa voz;
  late List<String> comandos;
  late http.Response Function(String comando) pc;

  setUp(() {
    Fallas.ultimo.value = null;
    oido = OidoFalso();
    voz = VozFalsa();
    comandos = [];
    pc = (c) => http.Response.bytes(utf8.encode(c.isEmpty ? 'falta el comando' : 'Abrí Spotify.'), c.isEmpty ? 400 : 200);
  });

  Widget app(AlmacenAjustes almacen, {Arranque? arranque}) => SokariApp(
        almacen: almacen,
        oido: oido,
        voz: voz,
        arranque: arranque,
        crearCliente: (a) => ClienteMalla(
          direccion: a.direccion,
          secreto: a.secreto,
          cliente: MockClient((r) async {
            final c = jsonDecode(utf8.decode(r.bodyBytes))['comando'] as String;
            comandos.add(c);
            return pc(c);
          }),
        ),
      );

  testWidgets('al abrirla escucha, le manda a la PC lo que dijiste y lee la respuesta', (t) async {
    await t.pumpWidget(app(AlmacenFalso(configurada)));
    await t.pumpAndSettle();
    expect(oido.escuchas, 1);
    expect(comandos, ['abre Spotify']);
    expect(find.text('abre Spotify'), findsOneWidget);
    expect(find.text('Abrí Spotify.'), findsOneWidget);
    expect(voz.dichos, ['Abrí Spotify.']);
    expect(find.text('Toca para hablar'), findsOneWidget);
  });

  testWidgets('sin configurar pide la PC, valida la dirección y guarda', (t) async {
    final almacen = AlmacenFalso(const Ajustes());
    await t.pumpWidget(app(almacen));
    await t.pumpAndSettle();
    expect(find.text('Conecta tu PC'), findsOneWidget);
    expect(oido.escuchas, 0);

    await t.tap(find.byKey(const Key('configurar')));
    await t.pumpAndSettle();
    await t.enterText(find.byKey(const Key('direccion')), '192.168.1.10');
    await t.pump();
    expect(find.textContaining('termina en .ts.net'), findsOneWidget);

    await t.enterText(find.byKey(const Key('direccion')), '100.64.0.7');
    await t.enterText(find.byKey(const Key('secreto')), 'secreto');
    await t.pump();
    await t.tap(find.byKey(const Key('probar')));
    await t.pumpAndSettle();
    expect(find.textContaining('Conectado'), findsOneWidget);
    expect(comandos, ['']);

    await t.tap(find.byKey(const Key('guardar')));
    await t.pumpAndSettle();
    expect(almacen.guardados, 1);
    expect(almacen.a.direccion, '100.64.0.7');
    expect(find.text('Toca para hablar'), findsOneWidget);
  });

  testWidgets('si la PC rechaza el secreto, lo dice y no lee nada', (t) async {
    pc = (_) => http.Response('secreto invalido', 401);
    await t.pumpWidget(app(AlmacenFalso(configurada)));
    await t.pumpAndSettle();
    expect(find.textContaining('secreto no coincide'), findsOneWidget);
    expect(voz.dichos, isEmpty);
  });

  testWidgets('también se le puede escribir', (t) async {
    await t.pumpWidget(app(AlmacenFalso(configurada.copiar(escucharAlAbrir: false))));
    await t.pumpAndSettle();
    expect(oido.escuchas, 0);
    await t.enterText(find.byKey(const Key('texto')), '¿qué hora es?');
    await t.tap(find.byKey(const Key('enviar')));
    await t.pumpAndSettle();
    expect(comandos, ['¿qué hora es?']);
    expect(find.text('Abrí Spotify.'), findsOneWidget);
  });

  testWidgets('si no te oyó, avisa sin mandar nada', (t) async {
    oido.error = 'error_no_match';
    await t.pumpWidget(app(AlmacenFalso(configurada)));
    await t.pumpAndSettle();
    expect(find.textContaining('No te oí'), findsOneWidget);
    expect(comandos, isEmpty);
  });

  testWidgets('con "leer en voz alta" apagado solo muestra la respuesta', (t) async {
    await t.pumpWidget(app(AlmacenFalso(configurada.copiar(leerRespuestas: false))));
    await t.pumpAndSettle();
    expect(find.text('Abrí Spotify.'), findsOneWidget);
    expect(voz.dichos, isEmpty);
  });

  testWidgets('cabe en un celular chico (360×640), también la configuración', (t) async {
    t.view.physicalSize = const Size(720, 1280);
    t.view.devicePixelRatio = 2;
    addTearDown(t.view.reset);
    await t.pumpWidget(app(AlmacenFalso(configurada)));
    await t.pumpAndSettle();
    expect(find.text('Abrí Spotify.'), findsOneWidget);
    await t.tap(find.byTooltip('Configuración'));
    await t.pumpAndSettle();
    await t.dragUntilVisible(find.text('Guardar'), find.byType(ListView).last, const Offset(0, -200));
    expect(find.text('Guardar'), findsOneWidget);
  });

  testWidgets('si los ajustes tardan en abrir, lo dice y deja configurar de nuevo (no solo el círculo)', (t) async {
    await t.pumpWidget(app(AlmacenLento()));
    await t.pump(const Duration(seconds: 1));
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    expect(find.byKey(const Key('lento')), findsNothing);
    await t.pump(const Duration(seconds: 3));
    expect(find.byKey(const Key('lento')), findsOneWidget);
    await t.tap(find.byKey(const Key('configurar_de_nuevo')));
    await t.pumpAndSettle();
    expect(find.text('Configuración'), findsOneWidget);
  });

  testWidgets('si los ajustes no se pueden leer, sigue y muestra el error con Copiar', (t) async {
    String? copiado;
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(SystemChannels.platform, (c) async {
      if (c.method == 'Clipboard.setData') copiado = (c.arguments as Map)['text'] as String;
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(SystemChannels.platform, null));
    await t.pumpWidget(app(AlmacenRoto()));
    await t.pumpAndSettle();
    expect(find.text('Conecta tu PC'), findsOneWidget);
    expect(find.byKey(const Key('falla')), findsOneWidget);
    expect(find.textContaining('No pude leer tus ajustes guardados'), findsOneWidget);
    await t.tap(find.byKey(const Key('copiar_falla')));
    await t.pumpAndSettle();
    expect(copiado, contains('AEADBadTagException'));
    await t.tap(find.byKey(const Key('cerrar_falla')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('falla')), findsNothing);
  });

  testWidgets('si no puede guardar, lo dice y se queda en Configuración', (t) async {
    await t.pumpWidget(app(AlmacenRoto()));
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('configurar')));
    await t.pumpAndSettle();
    await t.enterText(find.byKey(const Key('direccion')), '100.64.0.7');
    await t.enterText(find.byKey(const Key('secreto')), 'secreto');
    await t.pump();
    await t.tap(find.byKey(const Key('guardar')));
    await t.pumpAndSettle();
    expect(find.textContaining('No pude guardar tus ajustes'), findsWidgets);
    expect(find.byKey(const Key('guardar')), findsOneWidget);
  });

  testWidgets('lo que Android guardó de la vez pasada se ve al abrir', (t) async {
    await t.pumpWidget(app(AlmacenFalso(configurada.copiar(escucharAlAbrir: false)),
        arranque: ArranqueFalso('No alcanzó a mostrar su pantalla en 10 s (modo de dibujo: normal).')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('falla')), findsOneWidget);
    expect(find.textContaining('La vez pasada Sokari no abrió bien'), findsOneWidget);
    expect(find.text('Toca para hablar'), findsOneWidget);
  });

  testWidgets('sin nada de la vez pasada no aparece ningún aviso', (t) async {
    await t.pumpWidget(app(AlmacenFalso(configurada.copiar(escucharAlAbrir: false)), arranque: ArranqueFalso(null)));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('falla')), findsNothing);
  });

  test('un error se guarda con dónde pasó y su pila (corta)', () {
    Fallas.registrar(Exception('uy'), StackTrace.fromString(List.generate(30, (i) => '#$i f$i').join('\n')), 'Probando');
    final t = Fallas.ultimo.value!;
    expect(t, startsWith('Probando: Exception: uy'));
    expect(t, contains('#11 f11'));
    expect(t, isNot(contains('#12 f12')));
  });

  test('mensajes del reconocimiento de voz en español', () {
    expect(mensajeErrorOido('error_permission'), contains('permiso'));
    expect(mensajeErrorOido('error_network'), contains('internet'));
    expect(mensajeErrorOido('raro'), contains('raro'));
  });
}
