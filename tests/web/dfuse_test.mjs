// Проверка веб-прошивальщика без железа: протокол DfuSe, разбор ELF и адаптер WebUSB
// работают против эмулятора системного загрузчика STM32 (site/flash/*.js).
//
// Запуск:  node tests/web/dfuse_test.mjs
// Дополнительно (CI, после сборки прошивки):
//          node tests/web/dfuse_test.mjs build/Release/purrmidi.elf build/Release/purrmidi.bin
// сверяет образ, который собрал бы прошивальщик, с результатом arm-none-eabi-objcopy -O binary.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  DfuseDevice, FlashError, STATE, parseMemoryDescriptor, openWebUsbDfu, FLASH_BASE,
} from '../../site/flash/dfuse.js';
import { imageFromFile, buildImage, parseElfSegments } from '../../site/flash/elf.js';
import { FakeBootloader, SECTOR } from './fake_bootloader.mjs';

const H743_DESC = '@Internal Flash  /0x08000000/16*128Kg';

const noSleep = async () => {};
const makeDev = (boot, extra = {}) => new DfuseDevice(boot, parseMemoryDescriptor(H743_DESC), { sleep: noSleep, transferSize: boot.transferSize, ...extra });

function randomImage(size, seed = 1) {
  const d = new Uint8Array(size);
  let x = seed;
  for (let i = 0; i < size; i++) { x = (x * 1103515245 + 12345) >>> 0; d[i] = x >>> 24; }
  return d;
}

// Синтетический ELF с той же раскладкой, что у PurrMIDI (см. readelf -l): 4 сегмента во flash по LMA,
// два «.bss» без данных и сегмент ОЗУ без данных.
function makeElf(segments) {
  const phnum = segments.length;
  const dataStart = 52 + 32 * phnum;
  let size = dataStart;
  for (const s of segments) size += s.data.length;
  const buf = new Uint8Array(size);
  const v = new DataView(buf.buffer);
  buf.set([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1], 0);
  v.setUint16(0x10, 2, true); v.setUint16(0x12, 40, true); v.setUint32(0x14, 1, true);
  v.setUint32(0x18, 0x0801150d, true); v.setUint32(0x1c, 52, true);
  v.setUint16(0x28, 52, true); v.setUint16(0x2a, 32, true); v.setUint16(0x2c, phnum, true);
  let off = dataStart;
  segments.forEach((s, i) => {
    const p = 52 + 32 * i;
    v.setUint32(p, 1, true);
    v.setUint32(p + 4, s.data.length ? off : 0, true);
    v.setUint32(p + 8, s.vaddr ?? s.paddr, true);
    v.setUint32(p + 12, s.paddr, true);
    v.setUint32(p + 16, s.data.length, true);
    v.setUint32(p + 20, s.memsz ?? s.data.length, true);
    buf.set(s.data, off); off += s.data.length;
  });
  return buf;
}

let passed = 0;
async function test(name, fn) {
  try { await fn(); passed++; console.log('ok   ' + name); } catch (e) { console.error('FAIL ' + name + '\n' + (e.stack || e)); process.exitCode = 1; }
}

// ------------------------------------------------------------ тесты

await test('карта памяти H743', () => {
  const m = parseMemoryDescriptor(H743_DESC);
  assert.equal(m.name, 'Internal Flash');
  assert.equal(m.segments.length, 1); // одна группа секторов: 16 штук по 128 КБ
  assert.equal(m.segments[0].start, 0x08000000);
  assert.equal(m.segments[0].sectorSize, SECTOR);
  assert.equal(m.segments[0].end, 0x08200000);
  assert.ok(m.segments[0].readable && m.segments[0].erasable && m.segments[0].writable);
  const two = parseMemoryDescriptor('@Internal Flash  /0x08000000/08*128Kg,08*128Kg');
  assert.equal(two.segments.at(-1).end, 0x08200000);
  const ob = parseMemoryDescriptor('@Option Bytes  /0x5200201C/01*128 e');
  assert.equal(ob.segments[0].sectorSize, 128);
  assert.ok(!ob.segments[0].erasable && ob.segments[0].writable);
  assert.equal(parseMemoryDescriptor('что-то не то'), null);
  assert.equal(parseMemoryDescriptor('@Name /0xZZ/01*1Kg'), null);
});

await test('ELF: сегменты по p_paddr, склейка, выравнивание', () => {
  const code = randomImage(0x298, 1), itcm = randomImage(0x11d0, 2), text = randomImage(0x24b54, 3), data = randomImage(0x2644, 4);
  const elf = makeElf([
    { paddr: 0x08000000, data: code },
    { paddr: 0x08000298, vaddr: 0x00000000, data: itcm },
    { paddr: 0x08001480, data: text },
    { paddr: 0x08025fd4, vaddr: 0x20000000, data },
    { paddr: 0x08028618, vaddr: 0x20002648, data: new Uint8Array(0), memsz: 0x63f8 },
    { paddr: 0x24000000, data: new Uint8Array(0), memsz: 0x66000 },
  ]);
  assert.equal(parseElfSegments(elf).length, 4);
  const { kind, ranges } = imageFromFile(elf);
  assert.equal(kind, 'elf');
  assert.equal(ranges.length, 1);
  const r = ranges[0];
  assert.equal(r.address, 0x08000000);
  assert.equal(r.data.length % 32, 0);
  assert.deepEqual(r.data.subarray(0, 0x298), code);
  assert.deepEqual(r.data.subarray(0x298, 0x298 + 0x11d0), itcm);
  assert.deepEqual(r.data.subarray(0x1468, 0x1480), new Uint8Array(24).fill(0xff)); // зазор
  assert.deepEqual(r.data.subarray(0x1480, 0x1480 + 0x24b54), text);
  assert.deepEqual(r.data.subarray(0x25fd4, 0x25fd4 + 0x2644), data);
  assert.equal(r.data.length, Math.ceil(0x28618 / 32) * 32);
});

await test('ELF: ошибки (не ELF, не ARM, вне flash, пересечение)', () => {
  const bin = imageFromFile(Uint8Array.of(1, 2, 3, 4, 5).buffer, 0x08020000);
  assert.equal(bin.kind, 'bin');
  assert.equal(bin.ranges[0].address, 0x08020000);
  assert.equal(bin.ranges[0].data.length, 32);
  assert.throws(() => imageFromFile(new ArrayBuffer(0)), { code: 'empty_image' });
  const x86 = makeElf([{ paddr: 0x08000000, data: Uint8Array.of(1, 2, 3, 4) }]);
  new DataView(x86.buffer).setUint16(0x12, 3, true);
  assert.throws(() => imageFromFile(x86), { code: 'bad_elf' });
  const ram = makeElf([{ paddr: 0x24000000, data: Uint8Array.of(1, 2, 3, 4) }]);
  assert.throws(() => imageFromFile(ram), { code: 'outside_flash' });
  assert.throws(() => buildImage([{ address: 0x08000000, data: new Uint8Array(64) }, { address: 0x08000020, data: new Uint8Array(64) }]), { code: 'overlap' });
  assert.throws(() => imageFromFile(Uint8Array.of(1, 2, 3, 4).buffer, 0x20000000), { code: 'outside_flash' });
});

await test('прошивка: стирание только нужных секторов, запись, проверка, выход', async () => {
  const boot = new FakeBootloader();
  const dev = makeDev(boot);
  const img = randomImage(300 * 1024 + 8, 5); // 3 сектора
  const { ranges } = imageFromFile(img.buffer);
  const phases = new Set();
  const res = await dev.flash(ranges, { onProgress: (p) => phases.add(p.phase) });
  assert.equal(res.verified, true);
  assert.deepEqual(boot.erased, [0x08000000, 0x08020000, 0x08040000]);
  assert.deepEqual(boot.flash.subarray(0, img.length), img);
  assert.deepEqual(boot.flash.subarray(img.length, 3 * SECTOR), new Uint8Array(3 * SECTOR - img.length).fill(0xff));
  assert.equal(boot.flash[3 * SECTOR], (3 * SECTOR * 31 + 7) & 0xff); // чужие сектора не тронуты
  assert.ok(boot.gone, 'после записи плата должна выйти из загрузчика');
  assert.deepEqual([...phases], ['erase', 'write', 'verify', 'leave']);
});

await test('без проверки и без запуска', async () => {
  const boot = new FakeBootloader();
  const { ranges } = imageFromFile(randomImage(5000, 6).buffer);
  const res = await makeDev(boot).flash(ranges, { verify: false, leave: false });
  assert.equal(res.verified, false);
  assert.ok(!boot.gone);
});

await test('загрузчик в «грязном» состоянии (ERROR / DNLOAD_IDLE) приводится в IDLE', async () => {
  for (const st of [STATE.dfuERROR, STATE.dfuDNLOAD_IDLE, STATE.dfuUPLOAD_IDLE]) {
    const boot = new FakeBootloader();
    boot.state = st; if (st === STATE.dfuERROR) boot.status = 3;
    const { ranges } = imageFromFile(randomImage(2048, 7).buffer);
    const res = await makeDev(boot).flash(ranges);
    assert.equal(res.verified, true, 'state ' + st);
  }
});

await test('размер блока 2048 и нечётный 1000 (округляется до кратного 32)', async () => {
  for (const ts of [2048, 1000]) {
    const boot = new FakeBootloader({ transferSize: ts - (ts % 32) });
    const dev = makeDev(boot, { transferSize: ts });
    assert.equal(dev.transferSize % 32, 0);
    const img = randomImage(10000, 8);
    await dev.flash(imageFromFile(img.buffer).ranges);
    assert.deepEqual(boot.flash.subarray(0, img.length), img);
  }
});

await test('проверка нашла расхождение: ошибка с адресом, из загрузчика не выходим', async () => {
  const boot = new FakeBootloader({ corruptAt: 4321 });
  const { ranges } = imageFromFile(randomImage(8192, 9).buffer);
  await assert.rejects(makeDev(boot).flash(ranges), (e) => e.code === 'verify_mismatch' && e.params.address === FLASH_BASE + 4321);
  assert.ok(!boot.gone);
});

await test('чтение назад недоступно: предупреждение, прошивка продолжается', async () => {
  const boot = new FakeBootloader({ noUpload: true });
  const { ranges } = imageFromFile(randomImage(4096, 10).buffer);
  const warns = [];
  const res = await makeDev(boot).flash(ranges, { onWarning: (c) => warns.push(c) });
  assert.deepEqual(warns, ['verify_unavailable']);
  assert.equal(res.verified, false);
  assert.ok(boot.gone);
});

await test('адрес вне flash отклоняется до любых записей', async () => {
  const boot = new FakeBootloader();
  const dev = makeDev(boot);
  await assert.rejects(dev.flash([{ address: 0x20000000, data: new Uint8Array(32) }]), { code: 'out_of_flash' });
  await assert.rejects(dev.flash([{ address: 0x08000010, data: new Uint8Array(32) }]), { code: 'unaligned' });
  assert.deepEqual(boot.erased, []);
});

await test('ошибка стирания: понятный код, повторная попытка проходит', async () => {
  const boot = new FakeBootloader({ eraseError: true });
  const dev = makeDev(boot);
  const { ranges } = imageFromFile(randomImage(3000, 11).buffer);
  await assert.rejects(dev.flash(ranges), (e) => e.code === 'dfu_status' && e.params.status === 4);
  const res = await dev.flash(ranges);
  assert.equal(res.verified, true);
});

await test('зависший загрузчик: таймаут вместо бесконечного цикла', async () => {
  const boot = new FakeBootloader();
  boot.controlIn = async (req) => (req === 3 ? Uint8Array.of(0, 1, 0, 0, STATE.dfuDNBUSY, 0) : Uint8Array.of(STATE.dfuIDLE));
  let t = 0;
  const dev = makeDev(boot, { now: () => (t += 5000), waitLimitMs: 20000 });
  await assert.rejects(dev.command(0x41, FLASH_BASE), { code: 'timeout' });
});

await test('WebUSB: выбор интерфейса, wTransferSize из дескриптора, прошивка', async () => {
  const boot = new FakeBootloader({ transferSize: 2048 });
  const cfg = Uint8Array.of(
    9, 2, 27, 0, 1, 1, 0, 0xc0, 50,        // конфигурация, wTotalLength = 27
    9, 4, 0, 0, 0, 0xfe, 1, 2, 4,          // интерфейс DFU (протокол 2 = режим DFU)
    9, 0x21, 0x0b, 0xff, 0, 0x00, 0x08, 0x1a, 0x01, // функциональный дескриптор: wTransferSize = 0x0800
  );
  const dv = (u8) => new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const calls = [];
  const usb = {
    configuration: null,
    opened: false,
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    async selectConfiguration(n) {
      calls.push('cfg' + n);
      this.configuration = {
        interfaces: [{
          interfaceNumber: 0,
          alternates: [
            { alternateSetting: 0, interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 2, interfaceName: H743_DESC },
            { alternateSetting: 1, interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 2, interfaceName: '@Option Bytes  /0x5200201C/01*128 e' },
          ],
        }],
      };
    },
    async claimInterface(n) { calls.push('claim' + n); },
    async selectAlternateInterface(n, a) { calls.push(`alt${n}.${a}`); },
    async controlTransferIn(setup, length) {
      if (setup.requestType === 'standard') {
        assert.equal(setup.request, 6);
        const part = cfg.slice(0, length);
        return { status: 'ok', data: dv(part) };
      }
      assert.equal(setup.requestType, 'class'); assert.equal(setup.recipient, 'interface'); assert.equal(setup.index, 0);
      const d = await boot.controlIn(setup.request, setup.value, length);
      return { status: 'ok', data: dv(d) };
    },
    async controlTransferOut(setup, data) {
      assert.equal(setup.requestType, 'class'); assert.equal(setup.index, 0);
      const bytes = data ? new Uint8Array(data.buffer ?? data, data.byteOffset ?? 0, data.byteLength) : undefined;
      const n = await boot.controlOut(setup.request, setup.value, bytes);
      return { status: 'ok', bytesWritten: n };
    },
  };
  const { dfu, transferSize, altName } = await openWebUsbDfu(usb, FLASH_BASE, { sleep: noSleep });
  assert.equal(transferSize, 2048);
  assert.match(altName, /Internal Flash/);
  assert.deepEqual(calls, ['cfg1', 'claim0', 'alt0.0']);
  const img = randomImage(6000, 12);
  const res = await dfu.flash(imageFromFile(img.buffer).ranges);
  assert.equal(res.verified, true);
  assert.deepEqual(boot.flash.subarray(0, img.length), img);

  // устройство не в режиме загрузчика (протокол 1 = runtime) -> понятный код
  const rt = { ...usb, configuration: { interfaces: [{ interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 1, interfaceName: '' }] }] } };
  await assert.rejects(openWebUsbDfu(rt), { code: 'runtime_mode' });
  const none = { ...usb, configuration: { interfaces: [{ interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 3, interfaceSubclass: 0, interfaceProtocol: 0, interfaceName: '' }] }] } };
  await assert.rejects(openWebUsbDfu(none), { code: 'no_flash_interface' });
});

// ------------------------------------------------------------ реальная прошивка (CI)

const [elfPath, binPath] = process.argv.slice(2);
if (elfPath && binPath) {
  await test(`реальный ELF совпадает с objcopy -O binary (${elfPath})`, () => {
    const elf = fs.readFileSync(elfPath);
    const bin = fs.readFileSync(binPath);
    const { kind, ranges } = imageFromFile(elf.buffer.slice(elf.byteOffset, elf.byteOffset + elf.byteLength));
    assert.equal(kind, 'elf');
    assert.equal(ranges.length, 1);
    assert.equal(ranges[0].address, FLASH_BASE);
    const img = ranges[0].data;
    assert.ok(img.length - bin.length < 32 && img.length >= bin.length);
    // objcopy заполняет зазоры между сегментами нулями, прошивальщик - 0xFF (как в стёртой flash).
    // Поэтому сверяем байты сегментов с .bin, а всё остальное должно быть 0xFF.
    const covered = new Uint8Array(img.length);
    for (const seg of parseElfSegments(elf.buffer.slice(elf.byteOffset, elf.byteOffset + elf.byteLength))) {
      const off = seg.address - FLASH_BASE;
      assert.deepEqual(img.subarray(off, off + seg.data.length), seg.data);
      assert.deepEqual(new Uint8Array(bin.buffer, bin.byteOffset + off, seg.data.length), seg.data, 'сегмент отличается от .bin');
      covered.fill(1, off, off + seg.data.length);
    }
    for (let i = 0; i < img.length; i++) if (!covered[i]) assert.equal(img[i], 0xff, 'байт вне сегментов 0x' + i.toString(16));
    console.log(`     ELF ${elf.length} B -> образ ${ranges[0].data.length} B (bin ${bin.length} B)`);
  });
  await test('реальная прошивка проходит через эмулятор загрузчика', async () => {
    const elf = fs.readFileSync(elfPath);
    const { ranges } = imageFromFile(elf.buffer.slice(elf.byteOffset, elf.byteOffset + elf.byteLength));
    const boot = new FakeBootloader();
    const res = await makeDev(boot).flash(ranges);
    assert.equal(res.verified, true);
    assert.deepEqual(boot.flash.subarray(0, ranges[0].data.length), ranges[0].data);
  });
}

console.log(`\n${passed} тестов пройдено${process.exitCode ? ', ЕСТЬ ОШИБКИ' : ''}`);
