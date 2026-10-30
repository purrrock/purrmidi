// Веб-прошивальщик PurrMIDI: интерфейс страницы. Протокол - dfuse.js, разбор файла - elf.js.
// Язык берётся из <html lang>. Ничего не отправляется на сервер: файл читается и пишется в плату локально.

import { openWebUsbDfu, FlashError, ST_VID, ST_DFU_PID, FLASH_BASE } from './dfuse.js';
import { imageFromFile, imageSize, isElf } from './elf.js';

const lang = document.documentElement.lang === 'ru' ? 'ru' : 'en';
const $ = (id) => document.getElementById(id);

const T = {
  en: {
    latest: (tag, size) => `Latest release ${tag} (purrmidi.elf, ${size})`,
    fileInfo: (name, size, kind, addr) => (kind === 'elf'
      ? `${name}: ELF, ${size} to write to flash.`
      : `${name}: raw binary, ${size} to write at ${addr}.`),
    readErr: 'Could not read the file',
    badAddr: 'Enter the address as hex, for example 0x08000000 (flash starts at 0x08000000).',
    noFw: 'Choose a firmware file first.',
    unsupported: 'This browser cannot talk to USB devices (no WebUSB). Open this page in Chrome, Edge or another Chromium-based browser, or flash with STM32CubeProgrammer as described in the manual.',
    insecure: 'WebUSB works only on HTTPS pages. Open the site over https://.',
    chooser: 'Pick “STM32 BOOTLOADER” in the browser window…',
    cancelled: 'Cancelled: no device selected. If the list was empty, put the board into bootloader mode (see step 2) and try again.',
    connected: (n) => `Device: ${n}`,
    opened: (alt, ts) => `Interface: ${alt}, block size ${ts} B`,
    p_erase: (d, t) => `Erasing flash: sector ${Math.min(d + 1, t)} of ${t}…`,
    p_write: (d, t) => `Writing: ${kb(d)} of ${kb(t)}`,
    p_verify: (d, t) => `Verifying: ${kb(d)} of ${kb(t)}`,
    assumed: 'The browser did not report the interface name; assuming alt 0 = internal flash (as in the ST bootloader).',
    p_leave: () => 'Starting the firmware…',
    logErase: (n) => `Erased ${n} flash sector(s)`,
    logWrite: (n) => `Written ${kb(n)}`,
    logVerifyOk: 'Verification passed: flash matches the file',
    logLeave: 'Board left the bootloader',
    warnVerify: 'Could not read the flash back to verify it. The write itself reported no errors.',
    doneTitle: 'Done',
    doneLeave: 'The board restarted with the new firmware. On power-up the display shows a splash screen with the version number',
    doneVersion: (v) => ` (this one should read ${v})`,
    doneNoLeave: 'The firmware is written. Release BOOT0, then press NRST or replug the board to start it.',
    doneVerified: 'Flash was read back and matches the file.',
    again: 'To flash again, put the board back into bootloader mode.',
    failTitle: 'Flashing failed',
    busy: 'Flashing…',
    go: 'Connect and flash',
    e_runtime_mode: 'The device is in normal mode, not in the bootloader. Hold BOOT0 while plugging the board in (step 2).',
    e_no_flash_interface: 'This device has no internal-flash interface at the firmware address. Make sure it is the STM32 bootloader of a WeAct STM32H743 board.',
    e_usb_access: (d) => `The browser could not open the device (${d}). ${accessHint()}`,
    e_usb_lost: (d) => `Lost connection to the board (${d}). Use a short data cable without a hub, put the board into bootloader mode again and retry.`,
    e_usb_status: (p) => `The bootloader rejected a request (${p.status}). Put the board into bootloader mode again and retry.`,
    e_dfu_status: (p) => `The bootloader reported an error (code ${p.status}${p.status === 4 ? ': erase failed' : p.status === 3 ? ': write failed' : ''}). Put the board into bootloader mode again and retry.`,
    e_timeout: 'The bootloader stopped responding. Replug the board in bootloader mode and retry.',
    e_protocol: (p) => `Unexpected bootloader response (${p.detail}). Replug the board in bootloader mode and retry.`,
    e_verify_mismatch: (p) => `Verification failed: flash differs from the file at 0x${hex(p.address)}. The firmware may not work; flash again.`,
    e_out_of_flash: (p) => `Address 0x${hex(p.address)} is outside the board’s internal flash.`,
    e_outside_flash: (p) => `The file wants to write to 0x${hex(p.address)}, which is outside the internal flash (0x08000000–0x081FFFFF).`,
    e_unaligned: 'Image address or size is not a multiple of 32 bytes.',
    e_overlap: 'The ELF file has overlapping segments.',
    e_not_elf: 'This is not an ELF file.',
    e_bad_elf: (p) => `Unsuitable ELF file (${p.detail}). Use purrmidi.elf from the release.`,
    e_empty_image: 'The file is empty.',
    e_unknown: (m) => `Unexpected error: ${m}`,
    os: {
      win: 'On Windows Chrome can use the device only with the WinUSB driver: install it with Zadig for “STM32 BOOTLOADER” (see Requirements below).',
      linux: 'On Linux add the udev rule from the Requirements section below and replug the board.',
      other: 'Check the Requirements section below.',
    },
  },
  ru: {
    latest: (tag, size) => `Последний релиз ${tag} (purrmidi.elf, ${size})`,
    fileInfo: (name, size, kind, addr) => (kind === 'elf'
      ? `${name}: ELF, во flash будет записано ${size}.`
      : `${name}: сырой бинарник, ${size} будет записано по адресу ${addr}.`),
    readErr: 'Не удалось прочитать файл',
    badAddr: 'Введите адрес в шестнадцатеричном виде, например 0x08000000 (flash начинается с 0x08000000).',
    noFw: 'Сначала выберите файл прошивки.',
    unsupported: 'Этот браузер не умеет работать с USB-устройствами (нет WebUSB). Откройте страницу в Chrome, Edge или другом браузере на Chromium либо прошейте плату через STM32CubeProgrammer по инструкции.',
    insecure: 'WebUSB работает только на страницах HTTPS. Откройте сайт по адресу https://.',
    chooser: 'Выберите «STM32 BOOTLOADER» в окне браузера…',
    cancelled: 'Отменено: устройство не выбрано. Если список был пуст, переведите плату в режим загрузчика (шаг 2) и попробуйте снова.',
    connected: (n) => `Устройство: ${n}`,
    opened: (alt, ts) => `Интерфейс: ${alt}, размер блока ${ts} Б`,
    p_erase: (d, t) => `Стирание flash: сектор ${Math.min(d + 1, t)} из ${t}…`,
    p_write: (d, t) => `Запись: ${kb(d)} из ${kb(t)}`,
    p_verify: (d, t) => `Проверка: ${kb(d)} из ${kb(t)}`,
    assumed: 'Браузер не передал имя интерфейса; считаю, что alt 0 — внутренняя flash (так устроен загрузчик ST).',
    p_leave: () => 'Запуск прошивки…',
    logErase: (n) => `Стёрто секторов flash: ${n}`,
    logWrite: (n) => `Записано ${kb(n)}`,
    logVerifyOk: 'Проверка пройдена: flash совпадает с файлом',
    logLeave: 'Плата вышла из загрузчика',
    warnVerify: 'Прочитать flash обратно для проверки не удалось. Сама запись прошла без ошибок.',
    doneTitle: 'Готово',
    doneLeave: 'Плата перезапустилась с новой прошивкой. При включении на дисплее показывается заставка с номером версии',
    doneVersion: (v) => ` (у этой прошивки должно быть ${v})`,
    doneNoLeave: 'Прошивка записана. Отпустите BOOT0 и нажмите NRST или переподключите плату, чтобы она запустилась.',
    doneVerified: 'Flash прочитана обратно и совпадает с файлом.',
    again: 'Чтобы прошить ещё раз, снова переведите плату в режим загрузчика.',
    failTitle: 'Прошивка не удалась',
    busy: 'Идёт прошивка…',
    go: 'Подключиться и прошить',
    e_runtime_mode: 'Устройство в обычном режиме, а не в загрузчике. Зажмите BOOT0 при подключении платы (шаг 2).',
    e_no_flash_interface: 'У устройства нет интерфейса внутренней flash по адресу прошивки. Убедитесь, что это загрузчик STM32 платы WeAct STM32H743.',
    e_usb_access: (d) => `Браузер не смог открыть устройство (${d}). ${accessHint()}`,
    e_usb_lost: (d) => `Потеряна связь с платой (${d}). Возьмите короткий кабель для передачи данных без хаба, снова переведите плату в режим загрузчика и повторите.`,
    e_usb_status: (p) => `Загрузчик отклонил запрос (${p.status}). Переведите плату в режим загрузчика заново и повторите.`,
    e_dfu_status: (p) => `Загрузчик сообщил об ошибке (код ${p.status}${p.status === 4 ? ': не удалось стереть' : p.status === 3 ? ': не удалось записать' : ''}). Переведите плату в режим загрузчика заново и повторите.`,
    e_timeout: 'Загрузчик перестал отвечать. Переподключите плату в режиме загрузчика и повторите.',
    e_protocol: (p) => `Неожиданный ответ загрузчика (${p.detail}). Переподключите плату в режиме загрузчика и повторите.`,
    e_verify_mismatch: (p) => `Проверка не пройдена: flash отличается от файла по адресу 0x${hex(p.address)}. Прошивка может не работать, прошейте ещё раз.`,
    e_out_of_flash: (p) => `Адрес 0x${hex(p.address)} вне внутренней flash платы.`,
    e_outside_flash: (p) => `Файл хочет писать по адресу 0x${hex(p.address)}, это вне внутренней flash (0x08000000–0x081FFFFF).`,
    e_unaligned: 'Адрес или размер образа не кратен 32 байтам.',
    e_overlap: 'В ELF-файле пересекаются сегменты.',
    e_not_elf: 'Это не ELF-файл.',
    e_bad_elf: (p) => `Неподходящий ELF-файл (${p.detail}). Используйте purrmidi.elf из релиза.`,
    e_empty_image: 'Файл пуст.',
    e_unknown: (m) => `Непредвиденная ошибка: ${m}`,
    os: {
      win: 'В Windows Chrome может работать с устройством только через драйвер WinUSB: установите его программой Zadig для «STM32 BOOTLOADER» (см. «Требования» ниже).',
      linux: 'В Linux добавьте правило udev из раздела «Требования» ниже и переподключите плату.',
      other: 'Посмотрите раздел «Требования» ниже.',
    },
  },
}[lang];

// Исключения WebUSB (DOMException): отказ в доступе, отключение устройства и т.п. Остальное - не вина кабеля.
function isUsbError(e) {
  return !!e && ['NetworkError', 'NotFoundError', 'InvalidStateError', 'AbortError', 'SecurityError', 'NotAllowedError'].includes(e.name);
}
function hex(n) { return (n >>> 0).toString(16).toUpperCase().padStart(8, '0'); }
function kb(n) { return n >= 1024 ? `${(n / 1024).toFixed(n >= 10240 ? 0 : 1)} ${lang === 'ru' ? 'КБ' : 'KB'}` : `${n} ${lang === 'ru' ? 'Б' : 'B'}`; }
function accessHint() {
  const p = navigator.userAgent;
  return /Windows/.test(p) ? T.os.win : /Linux|X11|CrOS/.test(p) && !/Android/.test(p) ? T.os.linux : T.os.other;
}

// ------------------------------------------------------------ состояние

let fw = null;           // { name, ranges, kind, tag? }
let manifest = null;
let fileBuf = null;      // содержимое выбранного файла
let fileName = '';
let busy = false;

const el = {
  unsupported: $('unsupported'), latestRow: $('latest-row'), latest: $('src-latest'), latestLbl: $('lbl-latest'),
  file: $('file'), srcFile: $('src-file'), addrRow: $('addr-row'), addr: $('addr'), info: $('fw-info'),
  verify: $('opt-verify'), leave: $('opt-leave'), go: $('go'),
  progress: $('progress'), bar: $('bar'), status: $('status'), log: $('log'), logBox: $('log-box'),
  error: $('error'), result: $('result'),
};

function show(node, on) { node.hidden = !on; }
function text(node, s) { node.textContent = s; }
function log(line) { el.log.textContent += line + '\n'; el.log.scrollTop = el.log.scrollHeight; }

function message(e) {
  if (e instanceof FlashError) {
    const m = T['e_' + e.code];
    if (typeof m === 'function') return m(e.params);
    if (m) return m;
    return T.e_unknown(e.message);
  }
  return T.e_unknown((e && e.message) || String(e));
}

function setError(msg) {
  if (!msg) { show(el.error, false); return; }
  text(el.error, msg);
  show(el.error, true);
}

function updateGo() {
  el.go.disabled = busy || !fw || !!el.unsupported.dataset.blocked;
  el.go.textContent = busy ? T.busy : T.go;
}

// ------------------------------------------------------------ выбор прошивки

function parseAddr(s) {
  const m = /^\s*(?:0x)?([0-9a-fA-F]{1,8})\s*$/.exec(s);
  return m ? parseInt(m[1], 16) : null;
}

function applyFile() {
  fw = null;
  setError(null);
  text(el.info, '');
  if (!fileBuf) { updateGo(); return; }
  const elf = isElf(fileBuf);
  show(el.addrRow, !elf);
  try {
    let addr = FLASH_BASE;
    if (!elf) {
      addr = parseAddr(el.addr.value);
      if (addr === null) throw new FlashError('bad_address_input');
    }
    const { kind, ranges } = imageFromFile(fileBuf, addr);
    fw = { name: fileName, kind, ranges };
    text(el.info, T.fileInfo(fileName, kb(imageSize(ranges)), kind, '0x' + hex(ranges[0].address)));
  } catch (e) {
    setError(e.code === 'bad_address_input' ? T.badAddr : message(e));
  }
  updateGo();
}

async function useLatest() {
  setError(null);
  show(el.addrRow, false);
  text(el.info, '');
  if (!manifest) { fw = null; updateGo(); return; }
  fw = manifest.fw || null;
  if (fw) text(el.info, T.fileInfo('purrmidi.elf', kb(imageSize(fw.ranges)), 'elf'));
  updateGo();
}

async function loadManifest() {
  try {
    // Путь считается от самого модуля, а не от страницы: русская версия лежит на другом уровне (/ru/flash/).
    const r = await fetch(new URL('firmware/manifest.json', import.meta.url), { cache: 'no-cache' });
    if (!r.ok) throw new Error('no manifest');
    const m = await r.json();
    const f = await fetch(new URL('firmware/' + (m.file || 'purrmidi.elf'), import.meta.url), { cache: 'no-cache' });
    if (!f.ok) throw new Error('no firmware');
    const buf = await f.arrayBuffer();
    const { ranges, kind } = imageFromFile(buf);
    manifest = { ...m, fw: { name: m.file || 'purrmidi.elf', ranges, kind } };
    text(el.latestLbl, T.latest(m.tag, kb(buf.byteLength)));
    show(el.latestRow, true);
    el.latest.checked = true;
    await useLatest();
  } catch (e) {
    // Релиза ещё нет или файл недоступен: остаётся выбор своего файла.
    manifest = null;
    show(el.latestRow, false);
    el.srcFile.checked = true;
    updateGo();
  }
}

// ------------------------------------------------------------ прошивка

function overall(p, verify) {
  const w = verify ? { erase: 0.1, write: 0.5, verify: 0.4 } : { erase: 0.15, write: 0.85, verify: 0 };
  const base = { erase: 0, write: w.erase, verify: w.erase + w.write, leave: 1 };
  const frac = p.total ? p.done / p.total : 1;
  return base[p.phase] + (w[p.phase] || 0) * frac;
}

async function flash() {
  if (busy) return;
  if (!fw) { setError(T.noFw); return; }
  setError(null);
  show(el.result, false);
  el.log.textContent = '';
  const verify = el.verify.checked;
  const leave = el.leave.checked;

  // Окно выбора устройства должно открываться сразу по клику: прошивка уже загружена и разобрана.
  let usb;
  try {
    usb = await navigator.usb.requestDevice({ filters: [{ vendorId: ST_VID, productId: ST_DFU_PID }] });
  } catch (e) {
    if (e && e.name === 'NotFoundError') { text(el.status, T.cancelled); show(el.progress, true); el.bar.value = 0; return; }
    setError(T.e_usb_access(e && e.message || String(e)));
    return;
  }

  busy = true;
  updateGo();
  show(el.progress, true);
  el.bar.value = 0;
  text(el.status, '');
  show(el.logBox, true);
  const guard = (ev) => { ev.preventDefault(); ev.returnValue = ''; };
  window.addEventListener('beforeunload', guard);

  let stage = 'open';
  let warned = false;
  try {
    log(T.connected(usb.productName || 'STM32 BOOTLOADER'));
    const { dfu, altName, assumed, transferSize } = await openWebUsbDfu(usb, fw.ranges[0].address);
    log(T.opened(altName.trim(), transferSize));
    if (assumed) log(T.assumed);
    stage = 'flash';
    const res = await dfu.flash(fw.ranges, {
      verify, leave,
      onProgress: (p) => {
        el.bar.value = overall(p, verify);
        text(el.status, T['p_' + p.phase](p.done, p.total));
      },
      onWarning: () => { warned = true; log(T.warnVerify); },
    });
    log(T.logErase(res.sectors));
    log(T.logWrite(res.bytes));
    if (res.verified) log(T.logVerifyOk);
    if (leave) log(T.logLeave);
    el.bar.value = 1;
    showDone(res, leave, warned);
  } catch (e) {
    console.error(e);
    let msg;
    if (e instanceof FlashError) msg = message(e);
    else if (!isUsbError(e)) msg = T.e_unknown((e && e.message) || String(e));
    else if (stage === 'open') msg = T.e_usb_access((e && e.message) || String(e));
    else msg = T.e_usb_lost((e && e.message) || String(e));
    if (e && e.params && e.params.alts) for (const a of e.params.alts) log('  ' + a);
    log('✗ ' + msg);
    setError(msg);
    text(el.status, '');
  } finally {
    window.removeEventListener('beforeunload', guard);
    try { await usb.close(); } catch (e) { /* устройство уже ушло с шины после запуска программы */ }
    busy = false;
    updateGo();
  }
}

function showDone(res, leave, warned) {
  const ver = manifest && fw === manifest.fw ? T.doneVersion('v' + String(manifest.tag).replace(/^v/, '')) : '';
  const parts = [leave ? T.doneLeave + ver + '.' : T.doneNoLeave];
  if (res.verified) parts.push(T.doneVerified);
  if (warned) parts.push(T.warnVerify);
  parts.push(T.again);
  el.result.innerHTML = '';
  const h = document.createElement('h3');
  h.textContent = T.doneTitle;
  el.result.append(h);
  for (const s of parts) {
    const p = document.createElement('p');
    p.textContent = s;
    el.result.append(p);
  }
  show(el.result, true);
  text(el.status, '');
}

// ------------------------------------------------------------ старт

function init() {
  if (!window.isSecureContext || !('usb' in navigator)) {
    text(el.unsupported, window.isSecureContext ? T.unsupported : T.insecure);
    el.unsupported.dataset.blocked = '1';
    show(el.unsupported, true);
  }
  el.go.addEventListener('click', flash);
  el.latest.addEventListener('change', useLatest);
  el.srcFile.addEventListener('change', () => { if (fileBuf) applyFile(); else { fw = null; updateGo(); } });
  el.file.addEventListener('change', async () => {
    const f = el.file.files && el.file.files[0];
    if (!f) return;
    el.srcFile.checked = true;
    try {
      fileBuf = await f.arrayBuffer();
      fileName = f.name;
    } catch (e) {
      fileBuf = null;
      setError(T.readErr + ': ' + e.message);
      return;
    }
    applyFile();
  });
  el.addr.addEventListener('input', () => { if (fileBuf && !isElf(fileBuf)) applyFile(); });
  updateGo();
  loadManifest();
}

init();
