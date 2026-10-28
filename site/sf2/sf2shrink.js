/*
 * sf2shrink.js — уменьшение SoundFont2 в браузере: стерео -> моно + передискретизация.
 *
 * Это порт SoundFonts/sf2_shrink.py на JavaScript. Эталоном остаётся Python-скрипт:
 * алгоритм здесь повторяет его шаг в шаг (те же формулы, пороги и порядок операций), а
 * tests/web/sf2_parity.mjs в CI сравнивает результаты обоих реализаций. Меняете алгоритм в
 * одном месте — меняйте и в другом, иначе тест покажет расхождение.
 *
 * Работает без DOM: подключается через importScripts() в Web Worker и через require() в Node.
 * Файл не уходит на сервер, всё считается локально.
 *
 * Вместо FFT (как в numpy-версии) для поиска сдвига L/R используется прямая корреляция на
 * ±maxlag отсчётов: при maxlag=64 это дешевле FFT и даёт тот же результат.
 *
 * Ошибки (SF2Error) и предупреждения несут машинный код и параметры, чтобы интерфейс мог
 * показать текст на нужном языке; message — русский текст, как в CLI-версии.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.SF2Shrink = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  // ------------------------------------------------------------------------------------------
  //  Константы SF2
  // ------------------------------------------------------------------------------------------
  var GEN_PAN = 17;
  var GEN_SAMPLE_ID = 53;
  // Генераторы смещений адресов сэмпла: после передискретизации потеряли бы смысл.
  var GEN_ADDR_OFFSETS = [0, 1, 2, 3, 4, 12, 45, 50];

  var SAMPLE_MONO = 1, SAMPLE_RIGHT = 2, SAMPLE_LEFT = 4;
  var SAMPLE_VORBIS = 0x10, SAMPLE_ROM_FLAG = 0x8000;

  var SHDR_SIZE = 46;
  var PAD_SAMPLES = 46;

  /** Лимит банка в прошивке PurrMIDI, байт (8 МБ минус 4 КБ заголовка). */
  var DEVICE_LIMIT = 8384512;

  var DEFAULTS = {
    rate: 24000,
    gainDb: 0.0,
    taps: 96,
    lowpass: 0.96,
    maxlag: 64,
    minCorr: 0.4,
    align: true
  };

  // ------------------------------------------------------------------------------------------
  //  Ошибки
  // ------------------------------------------------------------------------------------------
  var MESSAGES = {
    not_sf2: function () { return "Это не SF2-файл (нет RIFF/sfbk)"; },
    chunk_corrupt: function (a) { return "Повреждённый чанк '" + a.id + "' (выходит за границы файла)"; },
    no_chunk: function (a) { return "Нет чанка " + a.name + " в pdta"; },
    no_smpl: function () { return "Нет чанка smpl (в файле нет PCM-данных)"; },
    rom_sample: function (a) { return "Сэмпл #" + a.index + " (" + a.name + ") — ROM-сэмпл, не поддерживается"; },
    compressed: function (a) { return "Сэмпл #" + a.index + " (" + a.name + ") сжат (SF3/Ogg Vorbis), не поддерживается"; },
    bad_range: function (a) { return "Сэмпл #" + a.index + " (" + a.name + "): некорректные start/end"; },
    addr_offset: function (a) {
      return "Зона #" + a.zone + " использует генератор смещения адреса сэмпла (" + a.gen + "): не поддерживается";
    },
    bad_sample_ref: function (a) { return "Зона #" + a.zone + " ссылается на несуществующий сэмпл " + a.id; },
    no_inst: function () { return "В банке нет инструментов (inst)"; },
    invalid_output: function (a) { return "Самопроверка результата не пройдена: " + a.detail; },
    big_endian: function () { return "Поддерживаются только little-endian платформы"; }
  };

  function SF2Error(code, args) {
    this.name = "SF2Error";
    this.code = code;
    this.args = args || {};
    this.message = MESSAGES[code] ? MESSAGES[code](this.args) : code;
    if (Error.captureStackTrace) Error.captureStackTrace(this, SF2Error);
  }
  SF2Error.prototype = Object.create(Error.prototype);
  SF2Error.prototype.constructor = SF2Error;

  // ------------------------------------------------------------------------------------------
  //  Вспомогательные функции
  // ------------------------------------------------------------------------------------------
  /** Округление к ближайшему чётному, как round() в Python 3 и numpy.rint. */
  function pyround(v) {
    var f = Math.floor(v);
    var d = v - f;
    if (d < 0.5) return f;
    if (d > 0.5) return f + 1;
    return (f % 2 === 0) ? f : f + 1;
  }

  function mod(a, n) {
    var r = a % n;
    return r < 0 ? r + n : r;
  }

  function bytesToString(u8, from, to) {
    var s = "";
    for (var i = from; i < to; i++) s += String.fromCharCode(u8[i]);
    return s;
  }

  function stringToBytes(s, len) {
    var out = new Uint8Array(len);
    for (var i = 0; i < s.length && i < len; i++) {
      var c = s.charCodeAt(i);
      out[i] = c < 256 ? c : 63; // '?', как errors="replace"
    }
    return out;
  }

  /** Нулевое значение генератора смещения (знаковое 16 бит). */
  function toSigned16(v) {
    return (v << 16) >> 16;
  }

  // ------------------------------------------------------------------------------------------
  //  RIFF / SF2: разбор
  // ------------------------------------------------------------------------------------------
  function readChunks(buf, dv, pos, end) {
    var out = [];
    while (pos + 8 <= end) {
      var id = bytesToString(buf, pos, pos + 4);
      var size = dv.getUint32(pos + 4, true);
      if (pos + 8 + size > end) throw new SF2Error("chunk_corrupt", { id: id });
      out.push({ id: id, off: pos + 8, size: size });
      pos += 8 + size + (size & 1);
    }
    return out;
  }

  function parseSF2(data) {
    var littleEndian = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1;
    if (!littleEndian) throw new SF2Error("big_endian");

    var buf = data instanceof Uint8Array ? data : new Uint8Array(data);
    var dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    if (buf.length < 12 || bytesToString(buf, 0, 4) !== "RIFF" || bytesToString(buf, 8, 12) !== "sfbk") {
      throw new SF2Error("not_sf2");
    }
    var riffSize = dv.getUint32(4, true);
    var end = Math.min(buf.length, 8 + riffSize);

    var sf = { infoPayload: null, smpl: null, hasSm24: false };
    var pdta = {};
    var top = readChunks(buf, dv, 12, end);
    for (var t = 0; t < top.length; t++) {
      var c = top[t];
      if (c.id !== "LIST") continue;
      var kind = bytesToString(buf, c.off, c.off + 4);
      var subs = readChunks(buf, dv, c.off + 4, c.off + c.size);
      if (kind === "INFO") {
        sf.infoPayload = buf.slice(c.off, c.off + c.size);
      } else if (kind === "sdta") {
        for (var s = 0; s < subs.length; s++) {
          var sc = subs[s];
          if (sc.id === "smpl") {
            var count = sc.size >> 1;
            var abs = buf.byteOffset + sc.off;
            sf.smpl = (abs % 2 === 0)
              ? new Int16Array(buf.buffer, abs, count)
              : new Int16Array(buf.slice(sc.off, sc.off + count * 2).buffer);
          } else if (sc.id === "sm24") {
            sf.hasSm24 = true;
          }
        }
      } else if (kind === "pdta") {
        for (var p = 0; p < subs.length; p++) {
          pdta[subs[p].id] = buf.subarray(subs[p].off, subs[p].off + subs[p].size);
        }
      }
    }

    var need = ["phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"];
    for (var k = 0; k < need.length; k++) {
      if (!pdta[need[k]]) throw new SF2Error("no_chunk", { name: need[k] });
    }
    if (!sf.smpl) throw new SF2Error("no_smpl");
    if (!sf.infoPayload) sf.infoPayload = new Uint8Array([0x49, 0x4e, 0x46, 0x4f]); // "INFO"

    sf.phdr = pdta.phdr; sf.pbag = pdta.pbag; sf.pmod = pdta.pmod; sf.pgen = pdta.pgen;

    // shdr
    var raw = pdta.shdr;
    var rdv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    var n = Math.floor(raw.length / SHDR_SIZE);
    if (n < 1) throw new SF2Error("no_chunk", { name: "shdr" });
    sf.shdr = [];
    for (var i = 0; i < n - 1; i++) {
      var o = i * SHDR_SIZE;
      sf.shdr.push({
        name: raw.slice(o, o + 20),
        start: rdv.getUint32(o + 20, true),
        end: rdv.getUint32(o + 24, true),
        ls: rdv.getUint32(o + 28, true),
        le: rdv.getUint32(o + 32, true),
        rate: rdv.getUint32(o + 36, true),
        pitch: rdv.getUint8(o + 40),
        corr: rdv.getInt8(o + 41),
        link: rdv.getUint16(o + 42, true),
        type: rdv.getUint16(o + 44, true)
      });
    }
    sf.shdrTerminal = raw.slice((n - 1) * SHDR_SIZE, n * SHDR_SIZE);

    // inst: (name20, bagNdx)
    raw = pdta.inst;
    rdv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    var nInst = Math.floor(raw.length / 22);
    sf.inst = [];
    for (i = 0; i < nInst; i++) {
      sf.inst.push({ name: raw.slice(i * 22, i * 22 + 20), bag: rdv.getUint16(i * 22 + 20, true) });
    }

    // ibag
    raw = pdta.ibag;
    rdv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    sf.ibag = [];
    for (i = 0; i < Math.floor(raw.length / 4); i++) {
      sf.ibag.push([rdv.getUint16(i * 4, true), rdv.getUint16(i * 4 + 2, true)]);
    }

    // igen: параллельные массивы операторов и сырых 16-битных значений (включая терминальную запись)
    raw = pdta.igen;
    rdv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    var nGen = Math.floor(raw.length / 4);
    sf.igenOp = new Uint16Array(nGen);
    sf.igenVal = new Uint16Array(nGen);
    for (i = 0; i < nGen; i++) {
      sf.igenOp[i] = rdv.getUint16(i * 4, true);
      sf.igenVal[i] = rdv.getUint16(i * 4 + 2, true);
    }
    sf.igenTerminal = raw.slice(Math.max(0, raw.length - 4));

    // imod: записи по 10 байт
    raw = pdta.imod;
    sf.imod = [];
    for (i = 0; i < Math.floor(raw.length / 10); i++) sf.imod.push(raw.subarray(i * 10, i * 10 + 10));
    sf.imodTerminal = raw.slice(Math.max(0, raw.length - 10));

    return sf;
  }

  function sampleName(sf, i) {
    var nm = sf.shdr[i].name;
    var e = 0;
    while (e < nm.length && nm[e] !== 0) e++;
    return bytesToString(nm, 0, e);
  }

  // ------------------------------------------------------------------------------------------
  //  Передискретизация: оконный sinc (Кайзер) с таблицей дробных фаз
  // ------------------------------------------------------------------------------------------
  /** Модифицированная функция Бесселя I0 (степенной ряд, точность ~1e-16 на |x| <= 20). */
  function besselI0(x) {
    var q = x * x / 4, term = 1, sum = 1;
    for (var k = 1; k < 200; k++) {
      term *= q / (k * k);
      sum += term;
      if (term < sum * 1e-17) break;
    }
    return sum;
  }

  function Interpolator(halfWidth, beta) {
    this.T = halfWidth | 0;
    this.beta = beta === undefined ? 9.0 : beta;
    this.NPHASE = 512;
    this._tables = {};
  }

  /** cutoff — частота среза в циклах на ИСХОДНЫЙ отсчёт (0..0.5). Таблица (NPHASE+1) x 2T. */
  Interpolator.prototype._table = function (cutoff) {
    var key = cutoff.toFixed(9);
    var tab = this._tables[key];
    if (tab) return tab;
    var T = this.T, NP = this.NPHASE, W = 2 * T;
    tab = new Float64Array((NP + 1) * W);
    var i0b = besselI0(this.beta);
    for (var f = 0; f <= NP; f++) {
      var frac = f / NP;
      var sum = 0, row = f * W;
      for (var t = 0; t < W; t++) {
        var tau = (-T + 1 + t) - frac;
        var u = 1.0 - (tau / T) * (tau / T);
        if (u < 0) u = 0; else if (u > 1) u = 1;
        var w = besselI0(this.beta * Math.sqrt(u)) / i0b;
        var arg = 2.0 * cutoff * tau;
        var sinc = arg === 0 ? 1.0 : Math.sin(Math.PI * arg) / (Math.PI * arg);
        var v = 2.0 * cutoff * sinc * w;
        tab[row + t] = v;
        sum += v;
      }
      for (t = 0; t < W; t++) tab[row + t] /= sum;      // единичное усиление на DC
    }
    this._tables[key] = tab;
    return tab;
  };

  /** Значения сигнала x в дробных позициях pos (в отсчётах x). */
  Interpolator.prototype.evaluate = function (x, pos, cutoff) {
    var T = this.T, NP = this.NPHASE, W = 2 * T;
    var tab = this._table(cutoff);
    var m = pos.length;
    var maxPos = 0;
    for (var k = 0; k < m; k++) if (pos[k] > maxPos) maxPos = pos[k];
    var need = m ? Math.floor(maxPos) + W + 2 : 0;
    var xpad = new Float64Array(Math.max(need, x.length + W + 2));
    xpad.set(x, T);                                       // xpad[T + i] = x[i]
    var out = new Float64Array(m);
    for (var s = 0; s < m; s++) {
      var p = pos[s];
      var i = Math.floor(p);
      var q = (p - i) * NP;
      var q0 = Math.floor(q);
      if (q0 > NP - 1) q0 = NP - 1;
      var a = q - q0, b = 1.0 - a;
      var r0 = q0 * W, r1 = r0 + W, base = i + 1;
      var acc = 0;
      for (var t = 0; t < W; t++) acc += xpad[base + t] * (tab[r0 + t] * b + tab[r1 + t] * a);
      out[s] = acc;
    }
    return out;
  };

  /**
   * x — Float64Array, исходный моно-сигнал; loop — [ls, le] относительно начала сэмпла или null.
   * Длина цикла в новой частоте — целое число, сетка подбирается так, чтобы начало цикла
   * попадало ровно в целый отсчёт: стык цикла остаётся бесшовным.
   */
  function resampleSample(interp, x, srcRate, tgtRate, loop, lowpass) {
    var n = x.length;
    var nominal = srcRate / tgtRate;                      // исходных отсчётов на один новый
    var cutoff = 0.5 / nominal * lowpass;
    var res = { ratio: 1.0 / nominal };
    var rho, phi, nNat, nOut, sp = 0, Lp = 0;

    if (loop) {
      var ls = loop[0], le = loop[1];
      var L = le - ls;
      Lp = Math.max(1, pyround(L / nominal));
      rho = L / Lp;
      sp = Math.floor(ls / rho);
      phi = sp - ls / rho;
      nNat = Math.ceil(n / rho + phi);
      nOut = Math.max(nNat, sp + Lp + 8);
    } else {
      rho = nominal; phi = 0.0;
      nNat = Math.ceil(n / nominal);
      nOut = nNat;
    }

    var pos = new Float64Array(nOut);
    for (var k = 0; k < nOut; k++) pos[k] = (k - phi) * rho;
    var y = interp.evaluate(x, pos, cutoff);

    if (loop) {
      for (k = Math.max(sp + Lp, nNat); k < nOut; k++) y[k] = y[k - Lp];
      res.loopStart = sp;
      res.loopEnd = sp + Lp;
    } else {
      res.loopStart = res.loopEnd = null;
    }
    res.data = y;
    res.centsErr = 1200.0 * Math.log2(rho / nominal);     // >0: тон после воспроизведения выше исходного
    return res;
  }

  // ------------------------------------------------------------------------------------------
  //  Сведение стереопары в моно с выравниванием каналов
  // ------------------------------------------------------------------------------------------
  /**
   * Лаг k (|k| <= maxlag) и знак, при которых a[i] и sign*b[i+k] максимально коррелируют.
   * Среди почти равных максимумов выбирается лаг с наименьшим |k|. Сдвиг применяется только если
   * он повышает |корреляцию| хотя бы на 0.1 относительно нулевого.
   */
  function bestLag(a, b, circular, maxlag, minCorr) {
    var n = a.length, i, k;
    if (n < 4 * maxlag + 16) return { lag: 0, sign: 1, corr: 0.0 };
    var aa = 0, bb = 0;
    for (i = 0; i < n; i++) { aa += a[i] * a[i]; bb += b[i] * b[i]; }
    var norm = Math.sqrt(aa * bb) + 1e-9;
    var M = maxlag, K = 2 * M + 1;

    // bext[j] = b[j - M] (циклически для зацикленной части, с нулями за краями для тела);
    // тогда c[lag] = sum a[i] * bext[i + lag + M]
    var bext = new Float64Array(n + 2 * M);
    for (var j = 0; j < bext.length; j++) {
      var src = j - M;
      if (circular) bext[j] = b[mod(src, n)];
      else bext[j] = (src >= 0 && src < n) ? b[src] : 0.0;
    }
    var cv = new Float64Array(K);
    for (k = 0; k < K; k++) {
      var s = 0;
      for (i = 0; i < n; i++) s += a[i] * bext[i + k];
      cv[k] = s / norm;
    }

    var m = 0;
    for (k = 0; k < K; k++) { var av = Math.abs(cv[k]); if (av > m) m = av; }
    var c0 = cv[M];
    if (m < minCorr) return { lag: 0, sign: 1, corr: c0 };
    if (m - Math.abs(c0) < 0.1) return { lag: 0, sign: c0 >= 0 ? 1 : -1, corr: c0 };
    var best = 0, bestAbs = Infinity;
    for (k = 0; k < K; k++) {
      if (Math.abs(cv[k]) >= m - 0.03) {
        var lg = k - M;
        if (Math.abs(lg) < bestAbs) { bestAbs = Math.abs(lg); best = lg; }
      }
    }
    var val = cv[best + M];
    return { lag: best, sign: val >= 0 ? 1 : -1, corr: val };
  }

  /** b[n + lag]; внутри цикла [ls, le) сдвиг циклический (сохраняет периодичность цикла). */
  function shifted(b, lag, loop) {
    var n = b.length;
    var out = new Float64Array(n);
    var ls = 0, le = 0, L = 0;
    if (loop) { ls = loop[0]; le = loop[1]; L = le - ls; }
    for (var p = 0; p < n; p++) {
      var idx = p + lag;
      if (loop && p >= ls && p < le) idx = ls + mod(p - ls + lag, L);
      if (idx >= 0 && idx < n) out[p] = b[idx];
    }
    return out;
  }

  /** Возвращает { data: left + выровненный right, info }. */
  function mixPair(left, right, loop, maxlag, minCorr, align) {
    var n = left.length, i;
    var out = new Float64Array(n);
    if (!align || maxlag <= 0) {
      for (i = 0; i < n; i++) out[i] = left[i] + right[i];
      return { data: out, info: { kind: "none" } };
    }
    if (loop && (loop[1] - loop[0]) >= 4 * maxlag + 16 && loop[0] > 0) {
      var ls = loop[0], le = loop[1];
      var bl = bestLag(left.subarray(ls, le), right.subarray(ls, le), true, maxlag, minCorr);
      var bb = bestLag(left.subarray(0, ls), right.subarray(0, ls), false, maxlag, minCorr);
      var rb = shifted(right, bb.lag, null);
      var rl = shifted(right, bl.lag, [ls, le]);
      var x = Math.min(2048, ls);
      var wStep = 1.0 / x;                                // как numpy.linspace(0, 1, x, endpoint=False)
      for (i = 0; i < n; i++) {
        var w;
        if (i < ls - x) w = 0.0;
        else if (i < ls) w = (i - (ls - x)) * wStep;      // плавный переход тело -> цикл
        else w = 1.0;
        out[i] = left[i] + (1.0 - w) * (bb.sign * rb[i]) + w * (bl.sign * rl[i]);
      }
      return { data: out, info: { kind: "body+loop", body: bb, loop: bl } };
    }
    var bw = bestLag(left, right, false, maxlag, minCorr);
    var rs = shifted(right, bw.lag, null);
    for (i = 0; i < n; i++) out[i] = left[i] + bw.sign * rs[i];
    return { data: out, info: { kind: "whole", whole: bw } };
  }

  // ------------------------------------------------------------------------------------------
  //  Основная логика
  // ------------------------------------------------------------------------------------------
  /** Группы сэмплов: стереопара (L, R) либо одиночный сэмпл. Возвращает { groups, gmap }. */
  function findGroups(sf) {
    var n = sf.shdr.length, i;
    for (i = 0; i < n; i++) {
      var t = sf.shdr[i].type;
      if (t & SAMPLE_ROM_FLAG) throw new SF2Error("rom_sample", { index: i, name: sampleName(sf, i) });
      if (t & SAMPLE_VORBIS) throw new SF2Error("compressed", { index: i, name: sampleName(sf, i) });
    }
    var partner = {};
    for (i = 0; i < n; i++) {
      if (sf.shdr[i].type === SAMPLE_LEFT) {
        var j = sf.shdr[i].link;
        if (j < n && j !== i && sf.shdr[j].type === SAMPLE_RIGHT && sf.shdr[j].link === i) {
          partner[i] = j;
          partner[j] = i;
        }
      }
    }
    var groups = [], gmap = new Int32Array(n).fill(-1);
    for (i = 0; i < n; i++) {
      if (gmap[i] >= 0) continue;
      var members = (i in partner) ? [i, partner[i]] : [i];
      if (members.length === 2 && sf.shdr[members[0]].type !== SAMPLE_LEFT) members = [members[1], members[0]];
      for (var m = 0; m < members.length; m++) gmap[members[m]] = groups.length;
      groups.push(members);
    }
    return { groups: groups, gmap: gmap };
  }

  function monoName(sf, members) {
    var name = sampleName(sf, members[0]);
    if (members.length === 2) {
      var r = sampleName(sf, members[1]);
      var last = name.slice(-1);
      if ((last === "L" || last === "l") && r.slice(0, -1) === name.slice(0, -1)) {
        name = name.slice(0, -1).replace(/[ _\-.]+$/, "");
      }
    }
    return name.slice(0, 19);
  }

  function rms(a) {
    var s = 0;
    for (var i = 0; i < a.length; i++) s += a[i] * a[i];
    return Math.sqrt(s / a.length);
  }

  function clampInt(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

  function chunk(id, payloadLen) {
    return { id: id, len: payloadLen, padded: payloadLen + (payloadLen & 1) };
  }

  /**
   * data — ArrayBuffer | Uint8Array исходного SF2. opts — см. DEFAULTS. onProgress({done,total,name}).
   * Возвращает { data: Uint8Array, stats }.
   */
  function shrink(data, opts, onProgress) {
    var o = Object.assign({}, DEFAULTS, opts || {});
    var inBytes = data.byteLength !== undefined ? data.byteLength : data.length;
    var sf = parseSF2(data);
    var smpl = sf.smpl;
    var nTotal = smpl.length;
    var fg = findGroups(sf), groups = fg.groups, gmap = fg.gmap;
    var gainLin = Math.pow(10.0, o.gainDb / 20.0);
    var interp = new Interpolator(o.taps);
    var tgt = o.rate;
    var z, i, g;

    // --- проверки зон ---
    for (z = 0; z < sf.ibag.length - 1; z++) {
      var g0 = sf.ibag[z][0], gEnd = sf.ibag[z + 1][0];
      var hasSid = false;
      for (g = g0; g < gEnd; g++) if (sf.igenOp[g] === GEN_SAMPLE_ID) hasSid = true;
      if (!hasSid) continue;
      for (g = g0; g < gEnd; g++) {
        if (GEN_ADDR_OFFSETS.indexOf(sf.igenOp[g]) >= 0 && toSigned16(sf.igenVal[g]) !== 0) {
          throw new SF2Error("addr_offset", { zone: z, gen: sf.igenOp[g] });
        }
      }
    }
    if (sf.inst.length < 1) throw new SF2Error("no_inst");

    // --- передискретизация каждой группы ---
    var results = [];
    for (var gi = 0; gi < groups.length; gi++) {
      var members = groups[gi];
      var L = sf.shdr[members[0]];
      var st = L.start, en = L.end, lsAbs = L.ls, leAbs = L.le, rate = L.rate;
      if (!(0 <= st && st < en && en <= nTotal)) {
        throw new SF2Error("bad_range", { index: members[0], name: sampleName(sf, members[0]) });
      }
      var name = monoName(sf, members);
      if (onProgress) onProgress({ done: gi, total: groups.length, name: name });

      var left = new Float64Array(en - st);
      for (i = 0; i < left.length; i++) left[i] = smpl[st + i];
      var warn = [], mixInfo = null, x;

      if (members.length === 2) {
        var R = sf.shdr[members[1]];
        if (!(0 <= R.start && R.start < R.end && R.end <= nTotal)) {
          throw new SF2Error("bad_range", { index: members[1], name: sampleName(sf, members[1]) });
        }
        var right = new Float64Array(R.end - R.start);
        for (i = 0; i < right.length; i++) right[i] = smpl[R.start + i];
        if (right.length !== left.length) {
          var mm = Math.min(left.length, right.length);
          warn.push({ code: "len_diff", l: left.length, r: right.length, m: mm });
          left = left.subarray(0, mm);
          right = right.subarray(0, mm);
        }
        if ((R.ls - R.start) !== (lsAbs - st) || (R.le - R.start) !== (leAbs - st)) warn.push({ code: "loop_diff" });
        if (R.rate !== rate) warn.push({ code: "rate_diff" });

        var mixLoop = (0 <= lsAbs - st && lsAbs - st < leAbs - st && leAbs - st <= left.length)
          ? [lsAbs - st, leAbs - st] : null;
        var mixed = mixPair(left, right, mixLoop, o.maxlag, o.minCorr, o.align);
        var rmsL = rms(left), rmsR = rms(right), rmsM = rms(mixed.data) + 1e-9;
        // суммарная мощность моно (по 0.707 на канал) = мощность пары
        var factor = Math.sqrt(rmsL * rmsL + rmsR * rmsR) / rmsM;
        if (!(0.25 <= factor && factor <= 2.0)) {
          warn.push({ code: "gain_clamped", factor: factor });
          factor = Math.min(2.0, Math.max(0.25, factor));
        }
        x = mixed.data;
        var mul = factor * gainLin;
        for (i = 0; i < x.length; i++) x[i] = x[i] * mul;
        mixInfo = mixed.info;
        mixInfo.factor = factor;
      } else {
        x = left;
      }
      var n = x.length;
      var ls = lsAbs - st, le = leAbs - st;
      var loop = (0 <= ls && ls < le && le <= n) ? [ls, le] : null;

      var res, outRate;
      if (rate > tgt) {
        res = resampleSample(interp, x, rate, tgt, loop, o.lowpass);
        outRate = tgt;
      } else {
        // уже достаточно низкая частота — оставляем как есть
        res = { data: x, ratio: 1.0, centsErr: 0.0, loopStart: loop ? loop[0] : null, loopEnd: loop ? loop[1] : null };
        outRate = rate;
      }
      if (!loop) {
        res.loopStart = Math.min(pyround(Math.max(0, ls) * res.ratio), res.data.length);
        res.loopEnd = Math.min(pyround(Math.max(0, le) * res.ratio), res.data.length);
      }

      // компенсация микро-расхождения шага через pitchCorrection
      var newCorr = clampInt(L.corr + pyround(-res.centsErr), -99, 99);
      var resid = res.centsErr + (newCorr - L.corr);

      results.push({
        members: members, name: name, res: res, rate: outRate, srcRate: rate, pitch: L.pitch, corr: newCorr,
        resid: resid, warn: warn, merged: members.length === 2, nSrc: n, loop: loop, mix: mixInfo
      });
    }

    // --- защита от клиппинга (равномерная для всех склеенных стереопар) ---
    var peak = 0.0, r, d;
    for (i = 0; i < results.length; i++) {
      r = results[i];
      if (!r.merged) continue;
      d = r.res.data;
      for (var q = 0; q < d.length; q++) { var av = Math.abs(d[q]); if (av > peak) peak = av; }
    }
    var guard = 1.0;
    if (peak > 32767.0 * 0.999) {
      guard = 32767.0 * 0.999 / peak;
      for (i = 0; i < results.length; i++) {
        r = results[i];
        if (!r.merged) continue;
        d = r.res.data;
        for (q = 0; q < d.length; q++) d[q] = d[q] * guard;
      }
    }

    // --- квантование и раскладка в новый smpl ---
    var newLen = 0;
    for (i = 0; i < results.length; i++) newLen += results[i].res.data.length + PAD_SAMPLES;
    var newSmpl = new Int16Array(newLen);
    var shdrBytes = new Uint8Array((results.length + 1) * SHDR_SIZE);
    var sdv = new DataView(shdrBytes.buffer);
    var pos = 0;
    for (i = 0; i < results.length; i++) {
      r = results[i];
      d = r.res.data;
      for (q = 0; q < d.length; q++) newSmpl[pos + q] = clampInt(pyround(d[q]), -32768, 32767);
      var start = pos, endp = pos + d.length;
      var lsN = start + r.res.loopStart, leN = start + r.res.loopEnd;
      var off = i * SHDR_SIZE;
      shdrBytes.set(stringToBytes(r.name, 20), off);
      sdv.setUint32(off + 20, start, true);
      sdv.setUint32(off + 24, endp, true);
      sdv.setUint32(off + 28, lsN, true);
      sdv.setUint32(off + 32, leN, true);
      sdv.setUint32(off + 36, r.rate, true);
      sdv.setUint8(off + 40, r.pitch);
      sdv.setInt8(off + 41, r.corr);
      sdv.setUint16(off + 42, 0, true);
      sdv.setUint16(off + 44, SAMPLE_MONO, true);
      r.start = start; r.end = endp; r.ls = lsN; r.le = leN;
      pos = endp + PAD_SAMPLES;
      r.nNew = d.length;
      r.res.data = null;                                  // память больше не нужна
    }
    shdrBytes.set(sf.shdrTerminal, results.length * SHDR_SIZE);

    // --- пересборка ibag / igen / imod ---
    var newIbag = [], newGenOp = [], newGenVal = [], newImod = [], newInst = [];
    var nRemoved = 0;
    var nInst = sf.inst.length - 1;                       // последняя запись inst — терминальная
    for (var ii = 0; ii < nInst; ii++) {
      var bag0 = sf.inst[ii].bag, bag1 = sf.inst[ii + 1].bag;
      newInst.push({ name: sf.inst[ii].name, bag: newIbag.length });
      var seen = {};
      for (z = bag0; z < bag1; z++) {
        var gA = sf.ibag[z][0], mA = sf.ibag[z][1];
        var gB = sf.ibag[z + 1][0], mB = sf.ibag[z + 1][1];
        var sid = -1;
        for (g = gA; g < gB; g++) if (sf.igenOp[g] === GEN_SAMPLE_ID) sid = sf.igenVal[g];
        var zoneMods = sf.imod.slice(mA, mB);
        if (sid < 0) {                                    // глобальная зона инструмента
          newIbag.push([newGenOp.length, newImod.length]);
          for (g = gA; g < gB; g++) { newGenOp.push(sf.igenOp[g]); newGenVal.push(sf.igenVal[g]); }
          for (i = 0; i < zoneMods.length; i++) newImod.push(zoneMods[i]);
          continue;
        }
        if (sid >= sf.shdr.length) throw new SF2Error("bad_sample_ref", { zone: z, id: sid });
        var gIdx = gmap[sid];
        var dropPan = results[gIdx].merged;
        if (dropPan) {
          var sig = gIdx + "|";
          for (g = gA; g < gB; g++) {
            if (sf.igenOp[g] !== GEN_PAN && sf.igenOp[g] !== GEN_SAMPLE_ID) sig += sf.igenOp[g] + ":" + sf.igenVal[g] + ",";
          }
          sig += "|";
          for (i = 0; i < zoneMods.length; i++) sig += Array.prototype.join.call(zoneMods[i], ".") + ";";
          if (seen[sig]) { nRemoved++; continue; }        // дубль зоны правого канала
          seen[sig] = true;
        }
        newIbag.push([newGenOp.length, newImod.length]);
        for (g = gA; g < gB; g++) {
          if (dropPan && sf.igenOp[g] === GEN_PAN) continue;
          newGenOp.push(sf.igenOp[g]);
          newGenVal.push(sf.igenOp[g] === GEN_SAMPLE_ID ? gIdx : sf.igenVal[g]);
        }
        for (i = 0; i < zoneMods.length; i++) newImod.push(zoneMods[i]);
      }
    }
    newInst.push({ name: sf.inst[sf.inst.length - 1].name, bag: newIbag.length });
    newIbag.push([newGenOp.length, newImod.length]);      // терминальная запись ibag

    var instB = new Uint8Array(newInst.length * 22);
    var idv = new DataView(instB.buffer);
    for (i = 0; i < newInst.length; i++) {
      instB.set(newInst[i].name, i * 22);
      idv.setUint16(i * 22 + 20, newInst[i].bag, true);
    }
    var ibagB = new Uint8Array(newIbag.length * 4);
    var bdv = new DataView(ibagB.buffer);
    for (i = 0; i < newIbag.length; i++) {
      bdv.setUint16(i * 4, newIbag[i][0], true);
      bdv.setUint16(i * 4 + 2, newIbag[i][1], true);
    }
    var igenB = new Uint8Array(newGenOp.length * 4 + sf.igenTerminal.length);
    var gdv = new DataView(igenB.buffer);
    for (i = 0; i < newGenOp.length; i++) {
      gdv.setUint16(i * 4, newGenOp[i], true);
      gdv.setUint16(i * 4 + 2, newGenVal[i], true);
    }
    igenB.set(sf.igenTerminal, newGenOp.length * 4);
    var imodB = new Uint8Array(newImod.length * 10 + sf.imodTerminal.length);
    for (i = 0; i < newImod.length; i++) imodB.set(newImod[i], i * 10);
    imodB.set(sf.imodTerminal, newImod.length * 10);

    // --- сборка файла ---
    var smplBytes = new Uint8Array(newSmpl.buffer, newSmpl.byteOffset, newSmpl.byteLength);
    var pdtaParts = [
      ["phdr", sf.phdr], ["pbag", sf.pbag], ["pmod", sf.pmod], ["pgen", sf.pgen],
      ["inst", instB], ["ibag", ibagB], ["imod", imodB], ["igen", igenB], ["shdr", shdrBytes]
    ];
    var pdtaLen = 4, k;
    for (k = 0; k < pdtaParts.length; k++) pdtaLen += 8 + chunk("", pdtaParts[k][1].length).padded;
    var infoLen = sf.infoPayload.length;
    var smplChunkPadded = chunk("", smplBytes.length).padded;
    var sdtaLen = 4 + 8 + smplChunkPadded;
    var bodyLen = 4 + (8 + infoLen + (infoLen & 1)) + (8 + sdtaLen) + (8 + pdtaLen);
    var out = new Uint8Array(8 + bodyLen);
    var odv = new DataView(out.buffer);
    var w = 0;
    function putId(s) { for (var c = 0; c < 4; c++) out[w++] = s.charCodeAt(c); }
    function putU32(v) { odv.setUint32(w, v, true); w += 4; }
    function putChunk(id, bytes) {
      putId(id); putU32(bytes.length); out.set(bytes, w); w += bytes.length + (bytes.length & 1);
    }
    putId("RIFF"); putU32(bodyLen); putId("sfbk");
    putChunk("LIST", sf.infoPayload);
    putId("LIST"); putU32(sdtaLen); putId("sdta"); putChunk("smpl", smplBytes);
    putId("LIST"); putU32(pdtaLen); putId("pdta");
    for (k = 0; k < pdtaParts.length; k++) putChunk(pdtaParts[k][0], pdtaParts[k][1]);

    var check = validate(out);
    if (onProgress) onProgress({ done: groups.length, total: groups.length, name: "" });

    // --- сводка ---
    var worst = 0.0, samples = [], warnings = [];
    for (i = 0; i < results.length; i++) {
      r = results[i];
      worst = Math.max(worst, Math.abs(r.resid));
      for (k = 0; k < r.warn.length; k++) {
        var wn = Object.assign({ sample: r.name }, r.warn[k]);
        warnings.push(wn);
      }
      samples.push({
        name: r.name, merged: r.merged, srcRate: r.srcRate, rate: r.rate, nSrc: r.nSrc, nNew: r.nNew,
        loopOld: r.loop, loopNew: [r.ls - r.start, r.le - r.start], cents: r.resid, mix: r.mix
      });
    }
    var stats = {
      inBytes: inBytes, outBytes: out.length, rate: tgt,
      groups: groups.length, merged: results.filter(function (e) { return e.merged; }).length,
      zonesRemoved: nRemoved, guard: guard, peak: peak, worstCents: worst,
      oldSamples: nTotal, newSamples: newLen, nSamples: check.samples, nZones: check.zones,
      sm24: sf.hasSm24, samples: samples, warnings: warnings
    };
    return { data: out, stats: stats };
  }

  /** Проверка базовых инвариантов SF2 в готовом файле. Возвращает { samples, zones }. */
  function validate(data) {
    function bad(detail) { throw new SF2Error("invalid_output", { detail: detail }); }
    var buf = data instanceof Uint8Array ? data : new Uint8Array(data);
    var sf = parseSF2(buf);
    var n = sf.smpl.length;
    var dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    if (dv.getUint32(4, true) !== buf.length - 8) bad("размер RIFF не совпадает с размером файла");
    var i, j;
    for (i = 0; i < sf.shdr.length; i++) {
      var h = sf.shdr[i];
      if (!(0 <= h.start && h.start < h.end && h.end <= n)) bad("shdr[" + i + "]: start/end вне smpl");
      if (h.end + PAD_SAMPLES > n) bad("shdr[" + i + "]: нет 46 нулевых отсчётов после сэмпла");
      for (j = 0; j < PAD_SAMPLES; j++) {
        if (sf.smpl[h.end + j] !== 0) bad("shdr[" + i + "]: нет 46 нулевых отсчётов после сэмпла");
      }
      if (!(h.start <= h.ls && h.ls <= h.le && h.le <= h.end)) bad("shdr[" + i + "]: цикл вне сэмпла");
      if (h.type !== SAMPLE_MONO || h.link !== 0) bad("shdr[" + i + "]: ожидался моно-сэмпл");
    }
    var nz = sf.ibag.length - 1;
    for (var z = 0; z < nz; z++) {
      if (sf.ibag[z][0] > sf.ibag[z + 1][0] || sf.ibag[z][1] > sf.ibag[z + 1][1]) bad("ibag: индексы не монотонны");
      for (var g = sf.ibag[z][0]; g < sf.ibag[z + 1][0]; g++) {
        if (sf.igenOp[g] === GEN_SAMPLE_ID && sf.igenVal[g] >= sf.shdr.length) bad("igen: sampleID вне диапазона");
      }
    }
    var last = sf.ibag[sf.ibag.length - 1];
    if (last[0] !== sf.igenOp.length - 1 || last[1] !== sf.imod.length - 1) {
      bad("ibag: терминальная запись указывает не на терминальные igen/imod");
    }
    return { samples: sf.shdr.length, zones: nz };
  }

  return {
    shrink: shrink,
    validate: validate,
    parseSF2: parseSF2,
    SF2Error: SF2Error,
    DEFAULTS: DEFAULTS,
    DEVICE_LIMIT: DEVICE_LIMIT
  };
});
