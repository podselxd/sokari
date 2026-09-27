import 'package:flutter/material.dart';

import 'ajustes.dart';
import 'fallas.dart';
import 'malla.dart';

class PantallaAjustes extends StatefulWidget {
  const PantallaAjustes({super.key, required this.ajustes, required this.almacen, required this.crearCliente});

  final Ajustes ajustes;
  final AlmacenAjustes almacen;
  final ClienteMalla Function(Ajustes a) crearCliente;

  @override
  State<PantallaAjustes> createState() => _PantallaAjustesState();
}

class _PantallaAjustesState extends State<PantallaAjustes> {
  late final _direccion = TextEditingController(text: widget.ajustes.direccion);
  late final _secreto = TextEditingController(text: widget.ajustes.secreto);
  late bool _escucharAlAbrir = widget.ajustes.escucharAlAbrir;
  late bool _leerRespuestas = widget.ajustes.leerRespuestas;
  bool _verSecreto = false;
  bool _probando = false;
  String? _resultado;
  bool _resultadoOk = false;

  @override
  void dispose() {
    _direccion.dispose();
    _secreto.dispose();
    super.dispose();
  }

  Ajustes get _actuales => widget.ajustes.copiar(
        direccion: _direccion.text.trim(),
        secreto: _secreto.text.trim(),
        escucharAlAbrir: _escucharAlAbrir,
        leerRespuestas: _leerRespuestas,
      );

  String? get _errorDireccion {
    final d = _direccion.text.trim();
    if (d.isEmpty || direccionPermitida(d)) return null;
    return 'Usa la IP 100.x.y.z de tu PC o su nombre que termina en .ts.net';
  }

  Future<void> _probar() async {
    setState(() {
      _probando = true;
      _resultado = null;
    });
    final cliente = widget.crearCliente(_actuales);
    try {
      await cliente.probar();
      _resultadoOk = true;
      _resultado = 'Conectado: tu PC contestó y el secreto coincide.';
    } on ErrorMalla catch (e) {
      _resultadoOk = false;
      _resultado = e.mensaje;
    } finally {
      cliente.cerrar();
    }
    if (mounted) setState(() => _probando = false);
  }

  Future<void> _guardar() async {
    final a = _actuales;
    try {
      await widget.almacen.guardar(a);
    } catch (e, s) {
      Fallas.registrar(e, s, 'No pude guardar tus ajustes');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('No pude guardar tus ajustes: $e')));
      }
      return;
    }
    if (mounted) Navigator.of(context).pop(a);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Configuración')),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            const Text(
              'En Sokari de tu PC, en Configuración → General, están la IP de Tailscale de tu PC y el secreto de '
              'malla. Este celular tiene que tener Tailscale abierto con la misma cuenta.',
              style: TextStyle(color: Colors.white70),
            ),
            const SizedBox(height: 16),
            TextField(
              key: const Key('direccion'),
              controller: _direccion,
              keyboardType: TextInputType.url,
              autocorrect: false,
              onChanged: (_) => setState(() => _resultado = null),
              decoration: InputDecoration(
                labelText: 'Dirección de tu PC',
                hintText: '100.x.y.z o mi-pc.tu-red.ts.net',
                errorText: _errorDireccion,
                border: const OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              key: const Key('secreto'),
              controller: _secreto,
              obscureText: !_verSecreto,
              autocorrect: false,
              enableSuggestions: false,
              onChanged: (_) => setState(() => _resultado = null),
              decoration: InputDecoration(
                labelText: 'Secreto de malla',
                border: const OutlineInputBorder(),
                suffixIcon: IconButton(
                  tooltip: _verSecreto ? 'Ocultar' : 'Mostrar',
                  icon: Icon(_verSecreto ? Icons.visibility_off : Icons.visibility),
                  onPressed: () => setState(() => _verSecreto = !_verSecreto),
                ),
              ),
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                FilledButton.tonal(
                  key: const Key('probar'),
                  onPressed: _probando || !_actuales.completos || _errorDireccion != null ? null : _probar,
                  child: const Text('Probar conexión'),
                ),
                const SizedBox(width: 12),
                if (_probando) const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
              ],
            ),
            if (_resultado != null) ...[
              const SizedBox(height: 8),
              Text(
                _resultado!,
                key: const Key('resultado'),
                style: TextStyle(color: _resultadoOk ? const Color(0xFF86EFAC) : const Color(0xFFFCA5A5)),
              ),
            ],
            const SizedBox(height: 16),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Escuchar al abrir la app'),
              value: _escucharAlAbrir,
              onChanged: (v) => setState(() => _escucharAlAbrir = v),
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Leer las respuestas en voz alta'),
              value: _leerRespuestas,
              onChanged: (v) => setState(() => _leerRespuestas = v),
            ),
            const SizedBox(height: 8),
            const Text(
              'Lo que borra o mueve archivos pide un “sí” de voz frente a la PC, así que desde aquí no se puede '
              'confirmar.',
              style: TextStyle(color: Colors.white54, fontSize: 13),
            ),
            const SizedBox(height: 20),
            FilledButton(
              key: const Key('guardar'),
              onPressed: _errorDireccion == null ? _guardar : null,
              child: const Text('Guardar'),
            ),
          ],
        ),
      ),
    );
  }
}
