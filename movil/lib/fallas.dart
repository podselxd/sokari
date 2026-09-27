// Que ningún error deje la app trabada sin decir nada: lo que truena se ve en
// pantalla (con «Copiar»), y lo que la tumbó la vez pasada (desde Android,
// ver MainActivity.kt y Arranque.kt) también.
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// Los errores que se ven en pantalla.
class Fallas {
  Fallas._();

  /// El último error (null: nada). La pantalla principal lo muestra.
  static final ultimo = ValueNotifier<String?>(null);

  static void registrar(Object error, StackTrace? pila, [String? donde]) {
    final texto = StringBuffer();
    if (donde != null) texto.write('$donde: ');
    texto.write(error);
    if (pila != null) {
      texto
        ..write('\n\n')
        ..write(pila.toString().split('\n').take(12).join('\n'));
    }
    ultimo.value = texto.toString();
    debugPrint('Sokari, error: $texto');
  }
}

/// Lo que Android guardó de la vez pasada (un error o que no alcanzó a dibujar).
abstract class Arranque {
  Future<String?> ultimoError();
}

class ArranqueAndroid implements Arranque {
  static const _canal = MethodChannel('sokari/arranque');

  @override
  Future<String?> ultimoError() async {
    try {
      return await _canal.invokeMethod<String>('ultimoError');
    } catch (_) {
      return null; // sin Android (las pruebas) no hay canal
    }
  }
}
