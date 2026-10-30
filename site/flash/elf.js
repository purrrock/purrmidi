// Превращает файл прошивки (.elf или .bin) в список диапазонов flash для DFU.
//
// ELF: берём загружаемые сегменты (PT_LOAD) по ФИЗИЧЕСКОМУ адресу p_paddr. У PurrMIDI .data и .itcm_text
// исполняются из ОЗУ (p_vaddr), но хранятся во flash сразу за кодом (p_paddr), поэтому нужен именно p_paddr.
// Сегменты без данных (.bss, буферы в ОЗУ) пропускаются.
//
// Соседние сегменты склеиваются (в зазорах между ними - 0xFF, как в стёртой flash), а границы выравниваются
// до 32 байт: слово flash у STM32H7 - 256 бит, неполное слово загрузчик может не принять.

import { FlashError, FLASH_BASE, FLASH_WORD } from './dfuse.js';

const PT_LOAD = 1;
const EM_ARM = 40;
const FLASH_SIZE = 2 * 1024 * 1024; // STM32H743VI
const MAX_GAP = 64 * 1024;          // зазор больше - отдельный диапазон, а не заполнение 0xFF

export function isElf(buf) {
  const d = new Uint8Array(buf);
  return d.length >= 4 && d[0] === 0x7f && d[1] === 0x45 && d[2] === 0x4c && d[3] === 0x46;
}

/** Сырые сегменты ELF: [{address, data}] (без склейки и выравнивания). */
export function parseElfSegments(buf) {
  const bytes = new Uint8Array(buf);
  if (!isElf(bytes)) throw new FlashError('not_elf');
  if (bytes.length < 52) throw new FlashError('bad_elf', { detail: 'short header' });
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes[4] !== 1 || bytes[5] !== 1) throw new FlashError('bad_elf', { detail: 'need ELF32 little-endian' });
  if (v.getUint16(0x12, true) !== EM_ARM) throw new FlashError('bad_elf', { detail: 'not an ARM image' });
  const phoff = v.getUint32(0x1c, true);
  const phentsize = v.getUint16(0x2a, true);
  const phnum = v.getUint16(0x2c, true);
  if (phentsize < 32 || phoff + phentsize * phnum > bytes.length) throw new FlashError('bad_elf', { detail: 'program headers' });

  const segs = [];
  for (let i = 0; i < phnum; i++) {
    const p = phoff + i * phentsize;
    if (v.getUint32(p, true) !== PT_LOAD) continue;
    const offset = v.getUint32(p + 4, true);
    const paddr = v.getUint32(p + 12, true);
    const filesz = v.getUint32(p + 16, true);
    if (filesz === 0) continue;
    if (offset + filesz > bytes.length) throw new FlashError('bad_elf', { detail: 'segment beyond file' });
    segs.push({ address: paddr, data: bytes.slice(offset, offset + filesz) });
  }
  if (!segs.length) throw new FlashError('bad_elf', { detail: 'no loadable segments' });
  return segs;
}

/**
 * Склеить и выровнять сегменты. Всё должно лежать во flash STM32H743 (0x08000000 + 2 МБ), иначе ошибка.
 * @returns [{address, data}] по возрастанию адреса, address и длина кратны 32
 */
export function buildImage(segments) {
  const sorted = [...segments].sort((a, b) => a.address - b.address);
  for (const s of sorted) {
    if (s.address < FLASH_BASE || s.address + s.data.length > FLASH_BASE + FLASH_SIZE) {
      throw new FlashError('outside_flash', { address: s.address, size: s.data.length });
    }
  }
  const groups = [];
  for (const s of sorted) {
    const last = groups[groups.length - 1];
    const end = s.address + s.data.length;
    if (last && s.address < last.end) throw new FlashError('overlap', { address: s.address });
    if (last && s.address - last.end <= MAX_GAP) {
      last.parts.push(s);
      last.end = end;
    } else {
      groups.push({ start: s.address, end, parts: [s] });
    }
  }
  const ranges = groups.map((g) => {
    const start = g.start - (g.start % FLASH_WORD);
    const end = Math.ceil(g.end / FLASH_WORD) * FLASH_WORD;
    const data = new Uint8Array(end - start).fill(0xff);
    for (const p of g.parts) data.set(p.data, p.address - start);
    return { address: start, data };
  });
  // Выравнивание могло сдвинуть границы соседних групп навстречу друг другу.
  for (let i = 1; i < ranges.length; i++) {
    if (ranges[i].address < ranges[i - 1].address + ranges[i - 1].data.length) throw new FlashError('overlap', { address: ranges[i].address });
  }
  return ranges;
}

/**
 * Файл -> диапазоны. Тип определяется по содержимому (сигнатура ELF), всё остальное считается сырым .bin,
 * который кладётся по адресу binAddress.
 */
export function imageFromFile(buf, binAddress = FLASH_BASE) {
  const bytes = new Uint8Array(buf);
  if (!bytes.length) throw new FlashError('empty_image');
  if (isElf(bytes)) return { kind: 'elf', ranges: buildImage(parseElfSegments(bytes)) };
  return { kind: 'bin', ranges: buildImage([{ address: binAddress, data: bytes }]) };
}

export function imageSize(ranges) {
  return ranges.reduce((s, r) => s + r.data.length, 0);
}
