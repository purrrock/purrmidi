// Эмулятор системного загрузчика STM32 (USB DFU, DfuSe) для проверок веб-прошивальщика без железа.
// Повторяет конечный автомат DFU: запрос в неподходящем состоянии даёт STALL и состояние dfuERROR.
// Flash ведёт себя как настоящая: писать можно только 1 -> 0, стирание секторное.

import { FlashError, STATE, FLASH_BASE } from '../../site/flash/dfuse.js';

export const SECTOR = 128 * 1024;

export class FakeBootloader {
  constructor(opts = {}) {
    this.transferSize = opts.transferSize || 1024;
    this.flash = new Uint8Array(16 * SECTOR);
    for (let i = 0; i < this.flash.length; i++) this.flash[i] = (i * 31 + 7) & 0xff; // «старая прошивка»
    this.state = STATE.dfuIDLE;
    this.status = 0;
    this.ptr = FLASH_BASE;
    this.pending = null;
    this.gone = false;
    this.erased = [];
    this.opts = opts;
  }

  fail(status = 0x0e) {
    this.state = STATE.dfuERROR;
    this.status = status;
    throw new FlashError('usb_status', { status: 'stall' });
  }

  async controlOut(request, value, data) {
    if (this.gone) throw new Error('device disconnected');
    const len = data ? data.length : 0;
    switch (request) {
      case 4: // CLRSTATUS
        if (this.state !== STATE.dfuERROR) this.fail();
        this.state = STATE.dfuIDLE; this.status = 0;
        return 0;
      case 6: // ABORT
        if (![STATE.dfuIDLE, STATE.dfuDNLOAD_SYNC, STATE.dfuDNLOAD_IDLE, STATE.dfuMANIFEST_SYNC, STATE.dfuUPLOAD_IDLE].includes(this.state)) this.fail();
        this.state = STATE.dfuIDLE; this.pending = null;
        return 0;
      case 1: { // DNLOAD
        if (this.state !== STATE.dfuIDLE && this.state !== STATE.dfuDNLOAD_IDLE) this.fail();
        if (len > this.transferSize) this.fail();
        if (len === 0) {
          if (this.state !== STATE.dfuDNLOAD_IDLE) this.fail();
          this.state = STATE.dfuMANIFEST_SYNC;
          return 0;
        }
        if (value === 0) this.pending = { cmd: data[0], addr: new DataView(data.buffer, data.byteOffset).getUint32(1, true) };
        else if (value >= 2) this.pending = { write: data.slice(), addr: this.ptr + (value - 2) * this.transferSize };
        else this.fail();
        this.state = STATE.dfuDNLOAD_SYNC;
        return len;
      }
      default:
        this.fail();
    }
    return 0;
  }

  execPending() {
    const p = this.pending;
    this.pending = null;
    if (p.cmd === 0x21) { this.ptr = p.addr; return 0; }
    if (p.cmd === 0x41) {
      if (this.opts.eraseError && !this.opts.eraseErrorDone) { this.opts.eraseErrorDone = true; return 4; } // errERASE
      const off = p.addr - FLASH_BASE;
      if (off < 0 || off % SECTOR || off >= this.flash.length) return 8; // errADDRESS
      this.flash.fill(0xff, off, off + SECTOR);
      this.erased.push(p.addr);
      return 0;
    }
    if (p.write) {
      const off = p.addr - FLASH_BASE;
      if (off < 0 || off + p.write.length > this.flash.length) return 8;
      for (let i = 0; i < p.write.length; i++) {
        let b = p.write[i];
        if (this.opts.corruptAt === off + i) b ^= 0x10;
        this.flash[off + i] &= b; // flash умеет только 1 -> 0
      }
      return 0;
    }
    return 0x0e;
  }

  async controlIn(request, value, length) {
    if (this.gone) throw new Error('device disconnected');
    if (request === 5) return Uint8Array.of(this.state);
    if (request === 3) {
      if (this.state === STATE.dfuDNLOAD_SYNC) {
        const st = this.execPending();
        if (st) { this.state = STATE.dfuERROR; this.status = st; } else this.state = STATE.dfuDNBUSY;
      } else if (this.state === STATE.dfuDNBUSY) {
        this.state = STATE.dfuDNLOAD_IDLE;
      } else if (this.state === STATE.dfuMANIFEST_SYNC) {
        this.gone = true; // перезагрузка в программу
        return Uint8Array.of(0, 0, 0, 0, STATE.dfuMANIFEST, 0);
      }
      return Uint8Array.of(this.status, 1, 0, 0, this.state, 0);
    }
    if (request === 2) { // UPLOAD
      if (this.opts.noUpload) this.fail();
      if (this.state !== STATE.dfuIDLE && this.state !== STATE.dfuUPLOAD_IDLE) this.fail();
      if (value < 2) this.fail();
      const off = this.ptr + (value - 2) * this.transferSize - FLASH_BASE;
      this.state = STATE.dfuUPLOAD_IDLE;
      return this.flash.slice(off, off + length);
    }
    this.fail();
    return null;
  }
}
