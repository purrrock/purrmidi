// Протокол DFU / DfuSe (ST) для системного загрузчика STM32 и адаптер WebUSB.
//
// Ядро (класс DfuseDevice) не знает про браузер: ему нужен объект-«транспорт» с двумя методами
//   controlOut(request, value, data?) -> Promise<число записанных байт>
//   controlIn(request, value, length) -> Promise<Uint8Array>
// Поэтому его можно проверять в Node на эмуляторе загрузчика (tests/web/dfuse_test.mjs).
// Адаптер WebUsbIo и функция openWebUsbDfu работают с настоящим USBDevice.
//
// Последовательности команд повторяют dfu-util (dfuse.c) и dfu.js (devanlai/webdfu, MIT).
// Тексты здесь не локализуются: ошибки несут код и параметры (FlashError), страница сама подбирает язык.

export const ST_VID = 0x0483;
export const ST_DFU_PID = 0xdf11;
export const FLASH_BASE = 0x08000000;

export class FlashError extends Error {
  constructor(code, params = {}, message) {
    super(message || code);
    this.name = 'FlashError';
    this.code = code;
    this.params = params;
  }
}

const REQ = { DNLOAD: 1, UPLOAD: 2, GETSTATUS: 3, CLRSTATUS: 4, GETSTATE: 5, ABORT: 6 };
export const STATE = {
  appIDLE: 0, appDETACH: 1, dfuIDLE: 2, dfuDNLOAD_SYNC: 3, dfuDNBUSY: 4, dfuDNLOAD_IDLE: 5,
  dfuMANIFEST_SYNC: 6, dfuMANIFEST: 7, dfuMANIFEST_WAIT_RESET: 8, dfuUPLOAD_IDLE: 9, dfuERROR: 10,
};
const CMD = { SET_ADDRESS: 0x21, ERASE: 0x41 };

// Размер слова flash у H7 - 32 байта: пишем и читаем кусками, кратными этому числу.
export const FLASH_WORD = 32;

// ---------------------------------------------------------------- карта памяти

/**
 * Разбор строки интерфейса DfuSe, например "@Internal Flash  /0x08000000/16*128Kg".
 * Возвращает { name, segments:[{start,end,sectorSize,readable,erasable,writable}] } или null.
 */
export function parseMemoryDescriptor(desc) {
  if (typeof desc !== 'string' || desc[0] !== '@') return null;
  const parts = desc.split('/');
  if (parts.length < 3) return null;
  const name = parts[0].slice(1).trim();
  const mult = { B: 1, K: 1024, M: 1048576, '': 1, ' ': 1 };
  const segments = [];
  for (let i = 1; i + 1 < parts.length; i += 2) {
    const a = /^\s*0x([0-9a-fA-F]{1,8})\s*$/.exec(parts[i]);
    if (!a) return null;
    let addr = parseInt(a[1], 16);
    for (const spec of parts[i + 1].split(',')) {
      const s = /^\s*(\d+)\s*\*\s*(\d+)\s*([BKM ]?)\s*([a-g])\s*$/.exec(spec);
      if (!s) return null;
      const count = parseInt(s[1], 10);
      const sectorSize = parseInt(s[2], 10) * mult[s[3]];
      // a=чтение, b=стирание, c=чтение+стирание, d=запись, e=чтение+запись, f=стирание+запись, g=всё
      const props = s[4].charCodeAt(0) - 96;
      if (!count || !sectorSize) return null;
      segments.push({
        start: addr, end: addr + count * sectorSize, sectorSize,
        readable: !!(props & 1), erasable: !!(props & 2), writable: !!(props & 4),
      });
      addr += count * sectorSize;
    }
  }
  return segments.length ? { name, segments } : null;
}

export function findSegment(map, address) {
  return map.segments.find((s) => address >= s.start && address < s.end) || null;
}

// ---------------------------------------------------------------- устройство

const H743_FALLBACK = '@Internal Flash  /0x08000000/16*128Kg';

const defaultSleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class DfuseDevice {
  /**
   * @param io        транспорт (см. шапку файла)
   * @param map       результат parseMemoryDescriptor для выбранного альт. интерфейса
   * @param opts      { transferSize, sleep, now }
   */
  constructor(io, map, opts = {}) {
    this.io = io;
    this.map = map;
    const t = opts.transferSize || 1024;
    this.transferSize = Math.max(FLASH_WORD, t - (t % FLASH_WORD));
    this.sleep = opts.sleep || defaultSleep;
    this.now = opts.now || Date.now;
    this.waitLimitMs = opts.waitLimitMs || 60000;
  }

  // ----- базовые запросы DFU

  async getStatus() {
    const d = await this.io.controlIn(REQ.GETSTATUS, 0, 6);
    if (d.length < 6) throw new FlashError('protocol', { detail: 'short GETSTATUS' });
    return { status: d[0], pollTimeout: d[1] | (d[2] << 8) | (d[3] << 16), state: d[4] };
  }

  async getState() {
    const d = await this.io.controlIn(REQ.GETSTATE, 0, 1);
    if (d.length < 1) throw new FlashError('protocol', { detail: 'short GETSTATE' });
    return d[0];
  }

  clearStatus() { return this.io.controlOut(REQ.CLRSTATUS, 0); }
  abort() { return this.io.controlOut(REQ.ABORT, 0); }

  async abortToIdle() {
    await this.abort();
    let state = await this.getState();
    if (state === STATE.dfuERROR) {
      await this.clearStatus();
      state = await this.getState();
    }
    if (state !== STATE.dfuIDLE) throw new FlashError('protocol', { detail: `state ${state} after abort` });
  }

  /** Привести загрузчик в dfuIDLE (после предыдущего сеанса или ошибки). */
  async init() {
    let st = await this.getStatus();
    if (st.state === STATE.dfuERROR) {
      await this.clearStatus();
      st = await this.getStatus();
    }
    if (st.state !== STATE.dfuIDLE) await this.abortToIdle();
  }

  /** Ждать нужного состояния, соблюдая bwPollTimeout. Ошибка устройства -> FlashError('dfu_status'). */
  async waitState(target) {
    const deadline = this.now() + this.waitLimitMs;
    let st = await this.getStatus();
    for (;;) {
      if (st.state === STATE.dfuERROR || (st.status !== 0 && st.state === target)) {
        throw new FlashError('dfu_status', { status: st.status, state: st.state });
      }
      if (st.state === target) return st;
      if (this.now() > deadline) throw new FlashError('timeout', { state: st.state });
      await this.sleep(st.pollTimeout);
      st = await this.getStatus();
    }
  }

  // ----- команды DfuSe

  async command(cmd, address) {
    const p = new Uint8Array(5);
    p[0] = cmd;
    new DataView(p.buffer).setUint32(1, address >>> 0, true);
    await this.io.controlOut(REQ.DNLOAD, 0, p);
    await this.waitState(STATE.dfuDNLOAD_IDLE);
  }

  setAddress(address) { return this.command(CMD.SET_ADDRESS, address); }

  async eraseSector(address) { await this.command(CMD.ERASE, address); }

  async writeChunk(address, data) {
    await this.setAddress(address);
    const n = await this.io.controlOut(REQ.DNLOAD, 2, data);
    if (n !== data.length) throw new FlashError('protocol', { detail: `wrote ${n} of ${data.length}` });
    await this.waitState(STATE.dfuDNLOAD_IDLE);
  }

  async readBlocks(address, length, onChunk) {
    await this.setAddress(address);
    await this.abortToIdle();
    const out = new Uint8Array(length);
    let off = 0;
    let block = 2;
    while (off < length) {
      const want = Math.min(this.transferSize, length - off);
      const d = await this.io.controlIn(REQ.UPLOAD, block++, want);
      if (d.length === 0) throw new FlashError('protocol', { detail: 'empty UPLOAD' });
      out.set(d.subarray(0, Math.min(d.length, length - off)), off);
      off += d.length;
      if (onChunk) onChunk(Math.min(off, length));
    }
    await this.abortToIdle();
    return out;
  }

  // ----- проверка диапазонов

  checkRanges(ranges) {
    for (const r of ranges) {
      const end = r.address + r.data.length;
      for (let a = r.address; a < end;) {
        const seg = findSegment(this.map, a);
        if (!seg || !seg.writable || !seg.erasable) throw new FlashError('out_of_flash', { address: a });
        a = Math.min(end, seg.end);
      }
      if (r.address % FLASH_WORD || r.data.length % FLASH_WORD) {
        throw new FlashError('unaligned', { address: r.address });
      }
    }
  }

  sectorsFor(ranges) {
    const set = new Map();
    for (const r of ranges) {
      const end = r.address + r.data.length;
      for (let a = r.address; a < end;) {
        const seg = findSegment(this.map, a);
        const start = seg.start + Math.floor((a - seg.start) / seg.sectorSize) * seg.sectorSize;
        set.set(start, seg.sectorSize);
        a = start + seg.sectorSize;
      }
    }
    return [...set.keys()].sort((x, y) => x - y);
  }

  // ----- главный сценарий

  /**
   * Стереть нужные сектора, записать диапазоны, (по желанию) проверить чтением и запустить программу.
   * @param ranges  [{address, data:Uint8Array}], адрес и длина кратны 32 (см. buildImage в elf.js)
   * @param opts    { verify, leave, onProgress({phase, done, total}), onWarning(code, params) }
   * @returns       { verified: true|false, sectors, bytes }
   */
  async flash(ranges, opts = {}) {
    const { verify = true, leave = true } = opts;
    const progress = opts.onProgress || (() => {});
    const warn = opts.onWarning || (() => {});
    if (!ranges.length) throw new FlashError('empty_image');
    this.checkRanges(ranges);
    await this.init();

    const sectors = this.sectorsFor(ranges);
    for (let i = 0; i < sectors.length; i++) {
      progress({ phase: 'erase', done: i, total: sectors.length, address: sectors[i] });
      await this.eraseSector(sectors[i]);
    }
    progress({ phase: 'erase', done: sectors.length, total: sectors.length });

    const total = ranges.reduce((s, r) => s + r.data.length, 0);
    let done = 0;
    progress({ phase: 'write', done, total });
    for (const r of ranges) {
      for (let off = 0; off < r.data.length; off += this.transferSize) {
        const chunk = r.data.subarray(off, Math.min(off + this.transferSize, r.data.length));
        await this.writeChunk(r.address + off, chunk);
        done += chunk.length;
        progress({ phase: 'write', done, total });
      }
    }

    let verified = false;
    if (verify) {
      let vdone = 0;
      progress({ phase: 'verify', done: 0, total });
      try {
        for (const r of ranges) {
          const base = vdone;
          const back = await this.readBlocks(r.address, r.data.length, (n) => progress({ phase: 'verify', done: base + n, total }));
          for (let i = 0; i < r.data.length; i++) {
            if (back[i] !== r.data[i]) throw new FlashError('verify_mismatch', { address: r.address + i });
          }
          vdone += r.data.length;
        }
        verified = true;
      } catch (e) {
        if (e instanceof FlashError && e.code === 'verify_mismatch') throw e;
        // Чтение назад не получилось (не ошибка записи): сообщаем и продолжаем.
        warn('verify_unavailable', { detail: String(e && e.message || e) });
        try { await this.init(); } catch (e2) { /* дальше всё равно попробуем выйти из загрузчика */ }
      }
    }

    if (leave) {
      progress({ phase: 'leave', done: 0, total: 1 });
      await this.leave(ranges[0].address);
      progress({ phase: 'leave', done: 1, total: 1 });
    }
    return { verified, sectors: sectors.length, bytes: total };
  }

  /** Выход из загрузчика: SET_ADDRESS + пустой DNLOAD + GETSTATUS (устройство перезагружается и пропадает с шины). */
  async leave(address) {
    await this.setAddress(address);
    await this.io.controlOut(REQ.DNLOAD, 0);
    try { await this.getStatus(); } catch (e) { /* ожидаемо: плата уже перезагрузилась */ }
  }
}

// ---------------------------------------------------------------- WebUSB

export class WebUsbIo {
  constructor(device, interfaceNumber) {
    this.device = device;
    this.iface = interfaceNumber;
  }

  async controlOut(request, value, data) {
    const setup = { requestType: 'class', recipient: 'interface', request, value, index: this.iface };
    const r = data && data.length ? await this.device.controlTransferOut(setup, data) : await this.device.controlTransferOut(setup);
    if (r.status !== 'ok') throw new FlashError('usb_status', { status: r.status, request });
    return r.bytesWritten;
  }

  async controlIn(request, value, length) {
    const setup = { requestType: 'class', recipient: 'interface', request, value, index: this.iface };
    const r = await this.device.controlTransferIn(setup, length);
    if (r.status !== 'ok') throw new FlashError('usb_status', { status: r.status, request });
    return new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
  }
}

/** wTransferSize из функционального дескриптора DFU (тип 0x21) в конфигурации; null, если прочитать не удалось. */
async function readTransferSize(device) {
  try {
    const setup = { requestType: 'standard', recipient: 'device', request: 0x06, value: 0x0200, index: 0 };
    const head = await device.controlTransferIn(setup, 4);
    if (head.status !== 'ok' || head.data.byteLength < 4) return null;
    const total = head.data.getUint16(2, true);
    const full = await device.controlTransferIn(setup, total);
    if (full.status !== 'ok') return null;
    const d = new Uint8Array(full.data.buffer, full.data.byteOffset, full.data.byteLength);
    for (let i = 0; i + 1 < d.length && d[i] > 0;) {
      if (d[i + 1] === 0x21 && d[i] >= 9) return d[i + 5] | (d[i + 6] << 8);
      i += d[i];
    }
  } catch (e) { /* используем значение по умолчанию */ }
  return null;
}

/**
 * Открыть выбранное пользователем устройство и найти альтернативный интерфейс с внутренней flash.
 * @returns { dfu: DfuseDevice, usb, altName, transferSize }
 */
export async function openWebUsbDfu(usbDevice, targetAddress = FLASH_BASE, opts = {}) {
  await usbDevice.open();
  try {
    if (usbDevice.configuration === null) await usbDevice.selectConfiguration(1);
    let runtime = false;
    let found = null;
    let fallback = null;   // DFU-интерфейс alt 0 без читаемого имени
    const seen = [];       // для журнала: что именно отдал браузер
    for (const iface of usbDevice.configuration.interfaces) {
      for (const alt of iface.alternates) {
        seen.push(`${iface.interfaceNumber}.${alt.alternateSetting} class=${alt.interfaceClass}/${alt.interfaceSubclass}/${alt.interfaceProtocol} name=${JSON.stringify(alt.interfaceName)}`);
        if (alt.interfaceClass !== 0xfe || alt.interfaceSubclass !== 0x01) continue;
        if (alt.interfaceProtocol !== 0x02) { runtime = true; continue; }
        const map = parseMemoryDescriptor(alt.interfaceName);
        if (map && findSegment(map, targetAddress)) {
          found = { iface: iface.interfaceNumber, alt: alt.alternateSetting, name: alt.interfaceName, map };
          break;
        }
        if (!map && alt.alternateSetting === 0 && !fallback) fallback = { iface: iface.interfaceNumber, alt: 0 };
      }
      if (found) break;
    }
    // Имя интерфейса не пришло или не разобралось (бывает в Chrome/Windows). У системного загрузчика STM32
    // alt 0 - всегда внутренняя flash, поэтому берём её со встроенной картой H743.
    if (!found && fallback && targetAddress >= FLASH_BASE && targetAddress < FLASH_BASE + 0x200000) {
      found = { ...fallback, name: H743_FALLBACK, map: parseMemoryDescriptor(H743_FALLBACK), assumed: true };
    }
    if (!found) throw new FlashError(runtime ? 'runtime_mode' : 'no_flash_interface', { alts: seen });

    await usbDevice.claimInterface(found.iface);
    await usbDevice.selectAlternateInterface(found.iface, found.alt);
    const ts = (await readTransferSize(usbDevice)) || 1024;
    const io = new WebUsbIo(usbDevice, found.iface);
    return { dfu: new DfuseDevice(io, found.map, { transferSize: ts, ...opts }), usb: usbDevice, altName: found.name, assumed: !!found.assumed, transferSize: ts };
  } catch (e) {
    try { await usbDevice.close(); } catch (e2) { /* игнорируем */ }
    throw e;
  }
}
