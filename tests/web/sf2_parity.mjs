// Сравнивает веб-версию уменьшения SF2 (site/sf2/sf2shrink.js) с эталоном SoundFonts/sf2_shrink.py.
//
// Для каждого банка и набора параметров запускает оба варианта и проверяет:
//   * все чанки, кроме smpl (заголовки пресетов, инструменты, зоны, генераторы, сэмплы shdr), совпадают побайтно;
//   * PCM в smpl той же длины и отличается не более чем на 1 LSB в ничтожной доле отсчётов
//     (суммирование во float идёт в другом порядке, поэтому редкие округления могут лечь иначе);
//   * ошибки (например, смещения адресов сэмпла) возникают в тех же случаях, что и в Python.
//
// Запуск:  node tests/web/sf2_parity.mjs            (нужны python3 и numpy)
import { createRequire } from "node:module";
import { execFileSync, spawnSync } from "node:child_process";
import { readFileSync, readdirSync, mkdtempSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, basename } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const SF2Shrink = require(join(root, "site", "sf2", "sf2shrink.js"));
const py = join(root, "SoundFonts", "sf2_shrink.py");
const tmp = mkdtempSync(join(tmpdir(), "sf2parity-"));

// Банки: маленькие и средние, чтобы CI укладывался в десятки секунд.
const MAX_BYTES = Number(process.env.SF2_PARITY_MAX_BYTES || 4_500_000);
const fixtures = [join(root, "tests", "SNDFNT.SF2")];
const flo = join(root, "SoundFonts", "Florestan");
if (existsSync(flo)) {
  for (const f of readdirSync(flo).sort()) {
    if (f.toLowerCase().endsWith(".sf2")) fixtures.push(join(flo, f));
  }
}
const variants = [
  { name: "default", js: {}, cli: [] },
  { name: "rate16k+gain", js: { rate: 16000, gainDb: 3.01 }, cli: ["--rate", "16000", "--gain-db", "3.01"] },
  { name: "no-align", js: { align: false }, cli: ["--no-align"] },
];

function walk(buf) {            // плоский список чанков "LIST/sub" -> байты
  const out = new Map();
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const id = (o) => String.fromCharCode(...buf.subarray(o, o + 4));
  (function rec(pos, end, prefix) {
    while (pos + 8 <= end) {
      const cid = id(pos), size = dv.getUint32(pos + 4, true);
      if (cid === "LIST") {
        const kind = id(pos + 8);
        out.set(prefix + "LIST:" + kind + ":head", buf.subarray(pos + 8, pos + 12));
        rec(pos + 12, pos + 8 + size, prefix + kind + "/");
      } else {
        out.set(prefix + cid, buf.subarray(pos + 8, pos + 8 + size));
      }
      pos += 8 + size + (size & 1);
    }
  })(12, buf.length, "");
  return out;
}

let failures = 0, runs = 0;
function fail(msg) { failures++; console.log("  НЕСОВПАДЕНИЕ: " + msg); }

for (const file of fixtures) {
  const size = readFileSync(file).length;
  if (size > MAX_BYTES) { console.log(`пропуск ${basename(file)} (${(size / 1e6).toFixed(1)} МБ > лимита теста)`); continue; }
  for (const v of variants) {
    runs++;
    const label = `${basename(file)} [${v.name}]`;
    const outPy = join(tmp, "py.sf2");
    const r = spawnSync("python3", [py, file, outPy, ...v.cli], { encoding: "utf-8" });
    let jsRes = null, jsErr = null;
    try { jsRes = SF2Shrink.shrink(readFileSync(file), v.js); } catch (e) { jsErr = e; }

    if (r.status !== 0) {                         // Python отказал — JS обязан отказать тем же
      if (!jsErr) { console.log(label); fail("Python вернул ошибку, а JS нет: " + r.stderr.trim()); continue; }
      const same = r.stderr.includes(jsErr.message.replace(/^Ошибка: /, ""));
      console.log(`${label}: отказ в обоих (${jsErr.code})${same ? "" : "  — тексты различаются"}`);
      if (!same) fail(`Python: ${r.stderr.trim()} | JS: ${jsErr.message}`);
      continue;
    }
    if (jsErr) { console.log(label); fail("JS вернул ошибку, а Python нет: " + jsErr.message); continue; }

    const a = new Uint8Array(readFileSync(outPy)), b = jsRes.data;
    const A = walk(a), B = walk(b);
    let bad = false;
    for (const key of new Set([...A.keys(), ...B.keys()])) {
      if (key.endsWith("smpl")) continue;
      const x = A.get(key), y = B.get(key);
      if (!x || !y || x.length !== y.length || Buffer.compare(x, y) !== 0) {
        if (!bad) console.log(label);
        bad = true;
        fail(`чанк ${key} различается (py ${x ? x.length : "-"} Б, js ${y ? y.length : "-"} Б)`);
      }
    }
    const sa = A.get("sdta/smpl"), sb = B.get("sdta/smpl");
    if (!sa || !sb || sa.length !== sb.length) {
      if (!bad) console.log(label);
      bad = true; fail("длина smpl различается");
    } else {
      const ia = new Int16Array(sa.buffer.slice(sa.byteOffset, sa.byteOffset + sa.length));
      const ib = new Int16Array(sb.buffer.slice(sb.byteOffset, sb.byteOffset + sb.length));
      let diff = 0, maxd = 0;
      for (let i = 0; i < ia.length; i++) {
        const d = Math.abs(ia[i] - ib[i]);
        if (d) { diff++; if (d > maxd) maxd = d; }
      }
      const frac = ia.length ? diff / ia.length : 0;
      console.log(`${label}: ${a.length} Б, чанки совпали; PCM: отличий ${diff}/${ia.length} (${(frac * 100).toFixed(4)}%), макс. ${maxd} LSB`);
      if (maxd > 1 || frac > 0.001) fail(`PCM расходится сильнее допуска (макс ${maxd} LSB, доля ${frac})`);
    }
  }
}
rmSync(tmp, { recursive: true, force: true });
console.log(`\nпрогонов: ${runs}, несовпадений: ${failures}`);
process.exit(failures ? 1 : 0);
