// Sokari en GNOME. En Wayland ningún programa puede ver las ventanas de los
// demás, oprimirles teclas ni leer el portapapeles, y está bien que así sea.
// Esta extensión le da a Sokari (y solo a él: el que está instalado en el
// sistema) lo justo para hacer lo que le pides con la voz: ver qué ventanas
// hay, traer una al frente, oprimir un atajo, pegar un texto y copiar un
// archivo. Las reglas de Sokari (no darle Enter a una terminal, avisar qué
// hizo) también se revisan aquí, por si alguien llamara sin pasar por él.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

Gio._promisify(Shell.Screenshot.prototype, 'screenshot_area');

// Se publica en la conexión del Shell, que ya es dueña de org.gnome.Shell:
// nadie más puede hacerse pasar por ella mientras el Shell corre.
const OBJECT_PATH = '/org/gnome/Shell/Extensions/Sokari';
const API_VERSION = 3;

const IFACE_XML = `<node>
  <interface name="io.github.podselxd.SokariShell1">
    <method name="Version">
      <arg type="u" direction="out" name="api"/>
      <arg type="s" direction="out" name="shell"/>
    </method>
    <method name="ListWindows"><arg type="s" direction="out" name="json"/></method>
    <method name="Focused"><arg type="s" direction="out" name="json"/></method>
    <method name="Desk"><arg type="s" direction="out" name="json"/></method>
    <method name="Screenshot">
      <arg type="s" direction="in" name="path"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="Activate">
      <arg type="t" direction="in" name="id"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="WindowAction">
      <arg type="t" direction="in" name="id"/>
      <arg type="s" direction="in" name="action"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="MinimizeAll"><arg type="u" direction="out" name="count"/></method>
    <method name="PressKeys">
      <arg type="t" direction="in" name="expect"/>
      <arg type="au" direction="in" name="keyvals"/>
      <arg type="u" direction="in" name="times"/>
      <arg type="s" direction="out" name="status"/>
    </method>
    <method name="Paste">
      <arg type="t" direction="in" name="expect"/>
      <arg type="s" direction="in" name="text"/>
      <arg type="s" direction="out" name="status"/>
    </method>
    <method name="GetClipboard"><arg type="s" direction="out" name="text"/></method>
    <method name="SetClipboard">
      <arg type="s" direction="in" name="text"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="SetClipboardFile">
      <arg type="s" direction="in" name="path"/>
      <arg type="s" direction="out" name="kind"/>
    </method>
    <method name="OwnWindow">
      <arg type="s" direction="in" name="title"/>
      <arg type="s" direction="in" name="action"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="b" direction="out" name="ok"/>
      <arg type="i" direction="out" name="out_x"/>
      <arg type="i" direction="out" name="out_y"/>
    </method>
    <method name="LaunchApp">
      <arg type="s" direction="in" name="desktop_id"/>
      <arg type="as" direction="in" name="uris"/>
      <arg type="s" direction="out" name="status"/>
    </method>
  </interface>
</node>`;

const KEY_ENTER = [0xff0d, 0xff8d, 0xfe34]; // Return, KP_Enter, ISO_Enter
const KEY_CONTROL_L = 0xffe3;
const KEY_V = 0x0076;
const MAX_KEYS = 6;
const MAX_TIMES = 20;
const MAX_IMAGE_BYTES = 15 * 1024 * 1024;

// Lo que suele ser una terminal aunque su .desktop no lo diga.
const TERMINAL_IDS = ['terminal', 'console', 'kgx', 'ptyxis', 'konsole', 'xterm', 'kitty', 'alacritty',
    'wezterm', 'foot', 'tilix', 'terminator', 'guake', 'tilda', 'terminology', 'rxvt', 'blackbox',
    'ghostty', 'warp', 'tabby', 'hyper', 'contour', 'yakuake', 'sakura', 'cool-retro-term'];

const TEXT_TYPES = ['text/plain', 'text/plain;charset=utf-8', 'UTF8_STRING', 'STRING', 'TEXT', 'COMPOUND_TEXT'];

function appOf(win) {
    return Shell.WindowTracker.get_default().get_window_app(win);
}

function categoriesOf(app) {
    try {
        return app?.get_app_info()?.get_categories() ?? '';
    } catch {
        return '';
    }
}

function isTerminal(win) {
    const app = appOf(win);
    if (categoriesOf(app).split(';').includes('TerminalEmulator'))
        return true;
    const ids = [app?.get_id(), win.get_wm_class(), win.get_gtk_application_id?.(), win.get_sandboxed_app_id?.()]
        .filter(Boolean).map(s => s.toLowerCase());
    return ids.some(id => TERMINAL_IDS.some(t => id.includes(t)));
}

function windowInfo(win) {
    const app = appOf(win);
    return {
        id: win.get_id(),
        app: app?.get_name() ?? '',
        app_id: app?.get_id() ?? '',
        wm_class: win.get_wm_class() ?? '',
        title: win.get_title() ?? '',
        categories: categoriesOf(app),
        pid: win.get_pid(),
        focused: win.has_focus(),
        minimized: win.minimized,
        above: win.is_above(),
        terminal: isTerminal(win),
    };
}

// Tus ventanas de todos los escritorios, la que usaste al último primero.
function userWindows() {
    return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null).filter(w => !w.is_skip_taskbar());
}

function findWindow(id) {
    return userWindows().find(w => w.get_id() === id) ?? null;
}

// La vista de actividades, «Ejecutar» (Alt+F2), un menú o un diálogo del
// Shell: ahí las teclas no van a ninguna ventana y Enter abre o ejecuta.
function shellHasFocus() {
    return Main.overview.visible || Main.modalCount > 0;
}

function sleep(ms) {
    return new Promise(resolve => {
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            resolve();
            return GLib.SOURCE_REMOVE;
        });
    });
}

function defaultSeat() {
    const backend = global.stage.get_context?.()?.get_backend?.() ?? Clutter.get_default_backend();
    return backend.get_default_seat();
}

function clipboardGet(clip, mimetype) {
    return new Promise(resolve => {
        if (mimetype)
            clip.get_content(St.ClipboardType.CLIPBOARD, mimetype, (_c, bytes) => resolve(bytes ?? null));
        else
            clip.get_text(St.ClipboardType.CLIPBOARD, (_c, text) => resolve(text ?? null));
    });
}

function textBytes(s) {
    return new GLib.Bytes(new TextEncoder().encode(s));
}

// Lo que había en el portapapeles, en un solo formato: GNOME no deja que una
// extensión ofrezca varios a la vez. El que más apps entienden.
const RESTORE_ORDER = ['image/png', 'image/jpeg', 'text/uri-list', 'x-special/gnome-copied-files', 'text/html'];

class SokariService {
    constructor() {
        this._kbd = defaultSeat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        this._allowed = new Map(); // quién llama (en el bus) -> su pid
        this._timeouts = new Set();
    }

    destroy() {
        for (const id of this._timeouts)
            GLib.source_remove(id);
        this._timeouts.clear();
        this._kbd?.run_dispose();
        this._kbd = null;
    }

    // ------------------------------------------------ quién puede llamar ---

    // Solo el programa sokari que instaló el sistema: un archivo de root que
    // nadie más puede cambiar. SOKARI_EXTENSION_ALLOW (en el entorno del
    // Shell, no en el de quien llama) es para las pruebas.
    _trusted(exe) {
        if (exe.endsWith(' (deleted)'))
            return false;
        const allow = GLib.getenv('SOKARI_EXTENSION_ALLOW');
        if (allow && allow.startsWith('/') && exe.startsWith(allow))
            return true;
        if (GLib.path_get_basename(exe) !== 'sokari')
            return false;
        const info = Gio.File.new_for_path(exe).query_info('unix::uid,unix::mode', Gio.FileQueryInfoFlags.NONE, null);
        return info.get_attribute_uint32('unix::uid') === 0 && (info.get_attribute_uint32('unix::mode') & 0o022) === 0;
    }

    _callerPid(sender) {
        return new Promise((resolve, reject) => {
            Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]), new GLib.VariantType('(u)'),
                Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                    try {
                        resolve(conn.call_finish(res).deepUnpack()[0]);
                    } catch (e) {
                        reject(e);
                    }
                });
        });
    }

    async _mayCall(sender) {
        if (this._allowed.has(sender))
            return true;
        try {
            const pid = await this._callerPid(sender);
            if (this._trusted(GLib.file_read_link(`/proc/${pid}/exe`))) {
                this._allowed.set(sender, pid);
                return true;
            }
        } catch (e) {
            console.warn(`Sokari: no pude revisar quién llama: ${e.message}`);
        }
        return false;
    }

    async _reply(invocation, signature, fn) {
        try {
            if (!await this._mayCall(invocation.get_sender())) {
                invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied',
                    'Esto solo lo puede usar Sokari (el que está instalado en el sistema).');
                return;
            }
            invocation.return_value(new GLib.Variant(signature, await fn()));
        } catch (e) {
            console.error(`Sokari: ${e.message}`);
            invocation.return_dbus_error('io.github.podselxd.SokariShell.Error', String(e?.message ?? e));
        }
    }

    // ---------------------------------------------------------- teclas ---

    _press(keyvals) {
        if (!this._kbd)
            throw new Error('La extensión se apagó');
        let t = GLib.get_monotonic_time();
        for (const k of keyvals)
            this._kbd.notify_keyval(t++, k, Clutter.KeyState.PRESSED);
        for (const k of [...keyvals].reverse())
            this._kbd.notify_keyval(t++, k, Clutter.KeyState.RELEASED);
    }

    // Por qué no se puede oprimir ahora ('' si sí se puede). expect: la
    // ventana que Sokari revisó (0: una tecla del sistema, como Alt+Tab).
    _blockedKeys(expect, enter) {
        if (Main.sessionMode.isLocked)
            return 'locked';
        const win = global.display.focus_window;
        const shell = shellHasFocus();
        if (expect && (shell || !win || win.get_id() !== expect))
            return 'focus';
        if (enter && shell)
            return 'shell';
        if (enter && win && isTerminal(win))
            return 'terminal';
        return '';
    }

    _blockedText(expect) {
        if (Main.sessionMode.isLocked)
            return 'locked';
        const win = global.display.focus_window;
        if (shellHasFocus() || !win || (expect && win.get_id() !== expect))
            return 'focus';
        return isTerminal(win) ? 'terminal' : '';
    }

    // Lo que tenías copiado, para devolvértelo después de pegar.
    async _saveClipboard(clip) {
        const types = clip.get_mimetypes(St.ClipboardType.CLIPBOARD);
        if (!types.length)
            return null;
        if (types.some(t => TEXT_TYPES.includes(t)) && types.every(t => TEXT_TYPES.includes(t) || t.startsWith('text/'))) {
            const text = await clipboardGet(clip, null);
            return text === null ? null : {text};
        }
        const mimetype = RESTORE_ORDER.find(t => types.includes(t)) ?? types[0];
        const bytes = await clipboardGet(clip, mimetype);
        return bytes ? {mimetype, bytes} : null;
    }

    _restoreLater(clip, ours, saved) {
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
            this._timeouts.delete(id);
            clip.get_text(St.ClipboardType.CLIPBOARD, (_c, now) => {
                // Si mientras tanto copiaste otra cosa, se queda lo tuyo.
                if (now !== ours)
                    return;
                if (saved.mimetype)
                    clip.set_content(St.ClipboardType.CLIPBOARD, saved.mimetype, saved.bytes);
                else
                    clip.set_text(St.ClipboardType.CLIPBOARD, saved.text);
            });
            return GLib.SOURCE_REMOVE;
        });
        this._timeouts.add(id);
    }

    // ----------------------------------------------------- los métodos ---

    VersionAsync(_params, invocation) {
        this._reply(invocation, '(us)', () => [API_VERSION, Config.PACKAGE_VERSION]);
    }

    ListWindowsAsync(_params, invocation) {
        this._reply(invocation, '(s)', () => [JSON.stringify(userWindows().map(windowInfo))]);
    }

    FocusedAsync(_params, invocation) {
        this._reply(invocation, '(s)', () => {
            const win = global.display.focus_window;
            const normal = win && !win.is_override_redirect() && !win.is_skip_taskbar();
            return [JSON.stringify({
                window: normal ? windowInfo(win) : null,
                shell_ui: shellHasFocus(),
                locked: Main.sessionMode.isLocked,
            })];
        });
    }

    // Para que la esfera flotante se mueva sola sin tapar nada: los monitores
    // (y lo que deja libre el panel), las ventanas de arriba a abajo, cuál
    // tiene el foco, el cursor y si algo está en pantalla completa. Sin
    // títulos ni contenido: solo dónde está cada cosa.
    DeskAsync(_params, invocation) {
        this._reply(invocation, '(s)', () => {
            const ws = global.workspace_manager.get_active_workspace();
            const monitors = Main.layoutManager.monitors.map((m, i) => {
                const w = ws.get_work_area_for_monitor(i);
                return {area: [m.x, m.y, m.width, m.height], work: [w.x, w.y, w.width, w.height]};
            });
            const pid = this._allowed.get(invocation.get_sender());
            const focus = global.display.focus_window;
            const wins = global.display.sort_windows_by_stacking(userWindows()).reverse()
                .filter(w => w.get_pid() !== pid && !w.minimized && w.located_on_workspace(ws))
                .map(w => {
                    const r = w.get_frame_rect();
                    return {r: [r.x, r.y, r.width, r.height], active: w === focus};
                });
            const [px, py] = global.get_pointer();
            const fullscreen = Boolean(focus && focus.is_fullscreen()) || Main.overview.visible;
            return [JSON.stringify({monitors, windows: wins, pointer: [px, py], fullscreen})];
        });
    }

    // «¿Qué ves en mi pantalla?»: solo cuando se lo pides a Sokari. El monitor
    // donde está el cursor, a un PNG en su carpeta de caché, sin sus ventanas.
    ScreenshotAsync([path], invocation) {
        this._reply(invocation, '(b)', async () => {
            const cache = GLib.get_user_cache_dir();
            if (!path.startsWith(`${cache}/sokari/`) || path.includes('..') || !path.endsWith('.png'))
                return [false];
            const pid = this._allowed.get(invocation.get_sender());
            const own = global.get_window_actors().filter(a => a.meta_window.get_pid() === pid && a.visible);
            own.forEach(a => a.hide());
            try {
                const [px, py] = global.get_pointer();
                const m = Main.layoutManager.monitors.find(mm => px >= mm.x && px < mm.x + mm.width &&
                    py >= mm.y && py < mm.y + mm.height) ?? Main.layoutManager.primaryMonitor;
                const file = Gio.File.new_for_path(path);
                const stream = file.replace(null, false, Gio.FileCreateFlags.PRIVATE, null);
                await new Shell.Screenshot().screenshot_area(m.x, m.y, m.width, m.height, stream);
                stream.close(null);
                return [true];
            } finally {
                own.forEach(a => a.show());
            }
        });
    }

    ActivateAsync([id], invocation) {
        this._reply(invocation, '(b)', () => {
            const win = findWindow(id);
            if (!win)
                return [false];
            if (Main.overview.visible)
                Main.overview.hide();
            Main.activateWindow(win);
            return [true];
        });
    }

    WindowActionAsync([id, action], invocation) {
        this._reply(invocation, '(b)', () => {
            const win = findWindow(id);
            if (!win)
                return [false];
            if (action === 'minimize') {
                if (!win.can_minimize())
                    return [false];
                win.minimize();
            } else if (action === 'maximize') {
                if (!win.can_maximize())
                    return [false];
                // Hasta GNOME 48 pide hacia dónde; desde el 49 ya no pide nada.
                win.maximize(Meta.MaximizeFlags?.BOTH ?? 3);
            } else if (action === 'close') {
                // Como darle a la X: si hay algo sin guardar, la app pregunta.
                win.delete(global.get_current_time());
            } else {
                return [false];
            }
            return [true];
        });
    }

    MinimizeAllAsync(_params, invocation) {
        this._reply(invocation, '(u)', () => {
            const ws = global.workspace_manager.get_active_workspace();
            let n = 0;
            for (const w of userWindows()) {
                if (w.minimized || !w.can_minimize() || !w.located_on_workspace(ws))
                    continue;
                w.minimize();
                n++;
            }
            return [n];
        });
    }

    PressKeysAsync([expect, keyvals, times], invocation) {
        this._reply(invocation, '(s)', async () => {
            if (!keyvals.length || keyvals.length > MAX_KEYS)
                return ['bad'];
            const enter = keyvals.some(k => KEY_ENTER.includes(k));
            times = Math.min(Math.max(times, 1), MAX_TIMES);
            for (let i = 0; i < times; i++) {
                if (i)
                    // eslint-disable-next-line no-await-in-loop
                    await sleep(60);
                const why = this._blockedKeys(expect, enter);
                if (why)
                    return [i ? `${why}:${i}` : why];
                this._press(keyvals);
            }
            return ['ok'];
        });
    }

    // Escribe pegando: así salen acentos, ñ y emojis con cualquier
    // distribución de teclado. Lo que tenías copiado vuelve a su lugar.
    PasteAsync([expect, text], invocation) {
        this._reply(invocation, '(s)', async () => {
            let why = this._blockedText(expect);
            if (why)
                return [why];
            const clip = St.Clipboard.get_default();
            const saved = await this._saveClipboard(clip);
            clip.set_text(St.ClipboardType.CLIPBOARD, text);
            // Que la app de enfrente ya tenga lo nuevo cuando llegue el Ctrl+V.
            await sleep(80);
            why = this._blockedText(expect);
            if (!why)
                this._press([KEY_CONTROL_L, KEY_V]);
            if (saved)
                this._restoreLater(clip, text, saved);
            return [why || 'ok'];
        });
    }

    GetClipboardAsync(_params, invocation) {
        this._reply(invocation, '(s)', async () => [await clipboardGet(St.Clipboard.get_default(), null) ?? '']);
    }

    SetClipboardAsync([text], invocation) {
        this._reply(invocation, '(b)', () => {
            St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, text);
            return [true];
        });
    }

    // Como «Copiar» en Archivos: pegado en un chat, se adjunta. Un PNG va
    // como imagen, que es lo que más apps aceptan pegar; lo demás, como la
    // lista de archivos que entienden los navegadores, Archivos y los chats.
    SetClipboardFileAsync([path], invocation) {
        this._reply(invocation, '(s)', async () => {
            const file = Gio.File.new_for_path(path);
            const [type] = Gio.content_type_guess(path, null);
            const clip = St.Clipboard.get_default();
            if (type === 'image/png') {
                const info = file.query_info('standard::size', Gio.FileQueryInfoFlags.NONE, null);
                if (info.get_size() <= MAX_IMAGE_BYTES) {
                    const [contents] = await new Promise((resolve, reject) => {
                        file.load_contents_async(null, (f, res) => {
                            try {
                                resolve(f.load_contents_finish(res).slice(1));
                            } catch (e) {
                                reject(e);
                            }
                        });
                    });
                    clip.set_content(St.ClipboardType.CLIPBOARD, 'image/png', new GLib.Bytes(contents));
                    return ['image'];
                }
            }
            clip.set_content(St.ClipboardType.CLIPBOARD, 'text/uri-list', textBytes(`${file.get_uri()}\r\n`));
            return ['uri-list'];
        });
    }

    // Solo ventanas del propio Sokari (de su pid y con ese título), nunca de
    // otros programas: la esfera flotante va encima de todo y en todos los
    // escritorios, y regresa a donde la dejaste (en Wayland una app no puede
    // ponerse encima ni moverse sola). Contesta dónde quedó.
    OwnWindowAsync([title, action, x, y], invocation) {
        this._reply(invocation, '(bii)', () => {
            const pid = this._allowed.get(invocation.get_sender());
            const win = global.get_window_actors().map(a => a.meta_window)
                .find(w => w.get_pid() === pid && w.get_title() === title);
            if (!win)
                return [false, 0, 0];
            if (action === 'above') {
                win.make_above();
                win.stick();
            } else if (action === 'normal') {
                win.unmake_above();
                win.unstick();
            } else if (action === 'move') {
                win.move_frame(true, x, y);
            } else if (action !== 'where') {
                return [false, 0, 0];
            }
            const r = win.get_frame_rect();
            return [true, r.x, r.y];
        });
    }

    // Abre la app o, si ya estaba abierta, trae su ventana: como el dock.
    // Con direcciones (una página, una carpeta), las abre en esa app. Desde
    // aquí la ventana nueva sale enfrente, como si la hubieras abierto tú.
    LaunchAppAsync([desktopId, uris], invocation) {
        this._reply(invocation, '(s)', () => {
            const app = Shell.AppSystem.get_default().lookup_app(desktopId);
            if (!app)
                return ['missing'];
            const running = app.get_state() === Shell.AppState.RUNNING && app.get_n_windows() > 0;
            if (Main.overview.visible)
                Main.overview.hide();
            if (uris.length) {
                const ctx = global.create_app_launch_context(0, -1);
                app.get_app_info().launch_uris(uris, ctx);
                return ['launched'];
            }
            app.activate();
            return [running ? 'focused' : 'launched'];
        });
    }
}

export default class SokariExtension extends Extension {
    enable() {
        this._service = new SokariService();
        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE_XML, this._service);
        this._dbus.export(Gio.DBus.session, OBJECT_PATH);
    }

    disable() {
        this._dbus?.unexport();
        this._dbus = null;
        this._service?.destroy();
        this._service = null;
    }
}
