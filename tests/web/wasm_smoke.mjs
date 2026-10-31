// Дымовой тест веб-демо: собранный Platform/web/build.sh модуль purrmidi.wasm должен звучать.
// Запуск: node tests/web/wasm_smoke.mjs [путь/к/purrmidi.wasm]   (нужен Node 18+)
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const wasmPath = process.argv[2] || path.join(root, "site/demo/purrmidi.wasm");
const bankPath = path.join(root, "SoundFonts/Florestan/014_Florestan_Tubular_Bells_and_glide.sf2");

let mem;
const wasi = {
  fd_close: () => 0,
  fd_seek: () => 0,
  fd_write: (fd, iov, n, nw) => {
    const dv = new DataView(mem.buffer);
    let t = 0;
    for (let i = 0; i < n; i++) t += dv.getUint32(iov + i * 8 + 4, true);
    dv.setUint32(nw, t, true);
    return 0;
  },
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { wasi_snapshot_preview1: wasi });
const x = instance.exports;
mem = x.memory;
x._initialize();
x.pm_init();

let failed = 0;
function check(ok, msg) {
  console.log(`${ok ? "ok  " : "FAIL"} ${msg}`);
  if (!ok) failed++;
}
function str(p) {
  const u = new Uint8Array(mem.buffer);
  let e = p;
  while (u[e]) e++;
  return Buffer.from(u.subarray(p, e)).toString();
}
function peakOf(blocks) {
  let peak = 0;
  for (let b = 0; b < blocks; b++) {
    const a = new Int16Array(mem.buffer, x.pm_render(128), 256);
    for (const v of a) peak = Math.max(peak, Math.abs(v));
  }
  return peak;
}

check(x.pm_program_count() >= 6, `программ в движке: ${x.pm_program_count()}`);
for (const [prog, name] of [[0, "E-Piano"], [1, "Pluck"], [2, "Lead"], [3, "Organ"], [4, "Violin"]]) {
  x.pm_select_program(prog);
  x.pm_note_on(60, 100);
  const p = peakOf(200);
  check(p > 1000, `${name} звучит (пик ${p})`);
  x.pm_panic();
  peakOf(50);
}

// Единое поведение движков: Pitch Bend, All Sound Off (CC120) и All Notes Off (CC123) во всех движках.
function render(blocks) {
  const out = [];
  for (let b = 0; b < blocks; b++) out.push(...new Int16Array(mem.buffer, x.pm_render(128), 256));
  return out;
}
const peak = (a) => a.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
for (const [prog, name] of [[0, "E-Piano"], [1, "Pluck"], [2, "Lead"], [3, "Organ"], [4, "Violin"]]) {
  x.pm_select_program(prog);
  x.pm_note_on(57, 100);
  render(100);
  const plain = render(40);
  x.pm_panic();
  x.pm_select_program(prog);
  x.pm_note_on(57, 100);
  render(100);
  x.pm_pitch_bend(8191);
  render(60);
  const bent = render(40);
  let diff = 0;
  for (let i = 0; i < plain.length; i++) diff += Math.abs(plain[i] - bent[i]);
  check(diff > plain.length * 50, `${name}: колесо высоты тона меняет звук`);
  x.pm_pitch_bend(0);
  x.pm_panic();

  x.pm_select_program(prog);
  x.pm_note_on(60, 110);
  x.pm_note_on(64, 110);
  const before = peak(render(200));
  x.pm_cc(120, 0);
  render(60);
  const afterKill = peak(render(20));
  check(before > 1000 && afterKill * 20 < before, `${name}: CC120 быстро гасит звук (${before} -> ${afterKill})`);
  x.pm_panic();

  x.pm_select_program(prog);
  x.pm_note_on(60, 110);
  x.pm_note_on(64, 110);
  const before2 = peak(render(200));
  x.pm_cc(123, 0);
  render(1500);
  const afterOff = peak(render(20));
  check(afterOff * 20 < before2, `${name}: CC123 отпускает удерживаемые ноты (${before2} -> ${afterOff})`);
  x.pm_panic();
}

x.pm_select_program(0);
[60, 64, 67].forEach((n) => x.pm_note_on(n, 100));
check(str(x.pm_chord()) === "C", `аккорд C распознан: «${str(x.pm_chord())}»`);
x.pm_panic();

const bank = fs.readFileSync(bankPath);
const ptr = x.pm_sf2_alloc(bank.length);
new Uint8Array(mem.buffer, ptr, bank.length).set(bank);
check(x.pm_sf2_commit() > 0, "банк SoundFont принят");
x.pm_select_program(5);
x.pm_note_on(72, 100);
const sfPeak = peakOf(300);
check(sfPeak > 1000, `SoundFont звучит (пик ${sfPeak})`);
x.pm_panic();

// Битый банк не должен ломать модуль и должен выключать SoundFont (иначе синтезатор читал бы освобождённую память)
const bad = new Uint8Array(5000);
bad.set([0x52, 0x49, 0x46, 0x46]);
const ptr2 = x.pm_sf2_alloc(bad.length);
new Uint8Array(mem.buffer, ptr2, bad.length).set(bad);
check(x.pm_sf2_commit() === 0, "битый банк отклонён");
x.pm_note_on(72, 100);
check(peakOf(100) === 0, "без банка SoundFont молчит");
x.pm_panic();
x.pm_select_program(0);
x.pm_note_on(60, 100);
check(peakOf(100) > 1000, "после битого банка E-Piano работает");

process.exit(failed ? 1 : 0);
