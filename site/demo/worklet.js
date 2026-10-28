// AudioWorklet: звуковые движки PurrMIDI (WebAssembly) работают прямо в аудиопотоке браузера.
// Все MIDI-вызовы и рендер идут в одном потоке, как требует контракт Engine/synth/synth_engine.h.
// Формат движка: 48 кГц, int16, стерео; здесь он переводится в float для Web Audio.

class PurrMidiProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.x = null;
    this.chord = "";
    // Движки считают на 48 кГц. Если контекст работает на другой частоте, блоки пересчитываются линейной
    // интерполяцией (запасной путь: обычно браузер честно даёт 48 кГц).
    this.ratio = 48000 / sampleRate;
    this.bl = new Float32Array(129);
    this.br = new Float32Array(129);
    this.pos = 128;

    const self = this;
    const wasi = {
      fd_close: () => 0,
      fd_seek: () => 0,
      // Журнал движков (stderr) в демо не нужен.
      fd_write: (fd, iov, n, nw) => {
        const dv = new DataView(self.x.memory.buffer);
        let total = 0;
        for (let i = 0; i < n; i++) total += dv.getUint32(iov + i * 8 + 4, true);
        dv.setUint32(nw, total, true);
        return 0;
      },
    };
    try {
      const mod = new WebAssembly.Module(options.processorOptions.wasm);
      const inst = new WebAssembly.Instance(mod, { wasi_snapshot_preview1: wasi });
      this.x = inst.exports;
      this.x._initialize();
      this.x.pm_init();
      this.port.onmessage = (e) => this.onMessage(e.data);
      this.port.postMessage({ t: "ready", rate: sampleRate });
      this.postState();
    } catch (err) {
      this.port.postMessage({ t: "error", message: String(err && err.message || err) });
    }
  }

  str(ptr) {
    const u = new Uint8Array(this.x.memory.buffer);
    let end = ptr;
    while (u[end]) end++;
    // TextDecoder в AudioWorkletGlobalScope недоступен: декодируем UTF-8 вручную.
    let out = "";
    for (let i = ptr; i < end; i++) {
      const c = u[i];
      if (c < 0x80) out += String.fromCharCode(c);
      else if (c >= 0xc0 && c < 0xe0 && i + 1 < end) { out += String.fromCharCode(((c & 31) << 6) | (u[++i] & 63)); }
      else if (c >= 0xe0 && c < 0xf0 && i + 2 < end) { out += String.fromCharCode(((c & 15) << 12) | ((u[++i] & 63) << 6) | (u[++i] & 63)); }
      else out += "?";
    }
    return out;
  }

  postState() {
    const x = this.x;
    const program = x.pm_program();
    this.port.postMessage({
      t: "state",
      program,
      count: x.pm_program_count(),
      name: this.str(x.pm_program_name(program)),
    });
  }

  postChord() {
    const c = this.str(this.x.pm_chord());
    if (c !== this.chord) {
      this.chord = c;
      this.port.postMessage({ t: "chord", text: c });
    }
  }

  loadBank(buf, id) {
    const x = this.x;
    // pm_sf2_alloc() сначала сбрасывает синтезатор SoundFont, поэтому смена банка безопасна и при активном SoundFont.
    let presets = [];
    const ptr = x.pm_sf2_alloc(buf.byteLength);
    if (ptr) {
      new Uint8Array(x.memory.buffer, ptr, buf.byteLength).set(new Uint8Array(buf));
      const n = x.pm_sf2_commit();
      for (let i = 0; i < n; i++) presets.push(this.str(x.pm_sf2_preset_name(i)));
    }
    this.port.postMessage({ t: "bank", id, presets });
    this.postState();
  }

  onMessage(m) {
    const x = this.x;
    switch (m.t) {
      case "on": x.pm_note_on(m.n, m.v); this.postChord(); break;
      case "off": x.pm_note_off(m.n); this.postChord(); break;
      case "cc": x.pm_cc(m.c, m.v); break;
      case "pb": x.pm_pitch_bend(m.v); break;
      case "at": x.pm_channel_pressure(m.v); break;
      case "prog": x.pm_select_program(m.p); this.postState(); this.postChord(); break;
      case "pc": x.pm_program_change(m.p); this.postState(); this.postChord(); break;
      case "panic": x.pm_panic(); this.postChord(); break;
      case "bank": this.loadBank(m.buf, m.id); break;
    }
  }

  // Рендер блока в 48 кГц во внутренние массивы bl/br (элемент 0 хранит последний отсчёт прошлого блока).
  renderBlock() {
    const x = this.x;
    const ptr = x.pm_render(128);
    const a = new Int16Array(x.memory.buffer, ptr, 256);   // память могла вырасти: представление берём заново
    this.bl[0] = this.bl[128];
    this.br[0] = this.br[128];
    for (let i = 0; i < 128; i++) {
      this.bl[i + 1] = a[2 * i] / 32768;
      this.br[i + 1] = a[2 * i + 1] / 32768;
    }
  }

  process(inputs, outputs) {
    const x = this.x;
    const out = outputs[0];
    if (!x || !out || out.length === 0) return true;
    const l = out[0];
    const r = out[1] || out[0];
    const n = l.length;

    if (this.ratio === 1) {
      const ptr = x.pm_render(n);
      const a = new Int16Array(x.memory.buffer, ptr, n * 2);
      for (let i = 0; i < n; i++) {
        l[i] = a[2 * i] / 32768;
        r[i] = a[2 * i + 1] / 32768;
      }
      return true;
    }

    for (let i = 0; i < n; i++) {
      if (this.pos >= 128) {
        this.pos -= 128;
        this.renderBlock();
      }
      const p0 = this.pos | 0;
      const f = this.pos - p0;
      l[i] = this.bl[p0] * (1 - f) + this.bl[p0 + 1] * f;
      r[i] = this.br[p0] * (1 - f) + this.br[p0 + 1] * f;
      this.pos += this.ratio;
    }
    return true;
  }
}

registerProcessor("purrmidi", PurrMidiProcessor);
