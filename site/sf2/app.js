/*
 * app.js — интерфейс онлайн-инструмента «Уменьшить SoundFont».
 * Одна логика для русской (sf2/) и английской (en/sf2/) страниц: язык берётся из <html lang>.
 * Вычисления идут в worker.js (sf2shrink.js), файл не покидает браузер.
 */
(function () {
  "use strict";

  var LANG = document.documentElement.lang === "en" ? "en" : "ru";
  var WORKER_URL = new URL("worker.js", document.currentScript.src).href;
  var DEVICE_LIMIT = 8384512;           // байт, как в прошивке (sf2_loader)
  var MAX_INPUT = 1536 * 1024 * 1024;   // выше браузер всё равно не потянет: нужен запас под результат

  var T = {
    ru: {
      pick: "Перетащите файл .sf2 сюда или нажмите, чтобы выбрать",
      picked: "Выбран файл:",
      change: "Выбрать другой файл",
      mb: "МБ",
      bytes: "байт",
      working: "Обработка…",
      go: "Уменьшить",
      cancel: "Отмена",
      cancelled: "Обработка отменена.",
      progress: function (a) {
        return (a.passes > 1 ? "Проход " + a.pass + " из " + a.passes + " · " : "") + a.rate + " Гц · сэмпл " +
          Math.min(a.done + 1, a.total) + " из " + a.total + (a.name ? " («" + a.name + "»)" : "");
      },
      fit: "Влезло в прошивку",
      nofit: "Не влезло в прошивку",
      sizes: function (a) {
        var t = "Было " + a.before + " МБ, стало " + a.after + " МБ";
        if (a.ratio >= 1.02) t += " (в " + a.ratioText + " раза меньше)";
        else if (a.ratio <= 0.98) t += " (стало больше)";
        else t += " (размер почти не изменился)";
        return t;
      },
      fileSize: function (n) { return "Размер файла: " + n + " байт."; },
      limit: "Лимит PurrMIDI: 8 384 512 байт (≈ 8 МБ).",
      download: "Скачать SNDFNT.SF2",
      downloadAs: function (n) { return "или сохранить как " + n; },
      nextFit: "Положите SNDFNT.SF2 в корень карты microSD (FAT32), вставьте карту и включите прибор.",
      nextNoFit: function (over) {
        return "Файл всё ещё больше лимита на " + over + " МБ. Что можно сделать: удалить ненужные пресеты и инструменты " +
          "в программе Polyphone и снова пропустить банк через этот инструмент, либо взять банк поменьше.";
      },
      alreadyFit: "Исходный файл уже помещался в лимит, так что сжатие было не обязательно.",
      tried: "Опробованные частоты:",
      did: "Что сделано",
      didRate: function (r) { return "Частота дискретизации: " + r + " Гц (сэмплы с более низкой частотой оставлены как есть)."; },
      didPairs: function (n, z) {
        return n ? "Стереопар сведено в моно: " + n + "; удалено дублирующих зон правого канала: " + z + "." :
          "Стереопар в банке нет, сведение в моно не потребовалось.";
      },
      didGuard: function (g) { return "Пик после сведения не помещался в 16 бит, склеенные сэмплы ослаблены в " + g + " раза (защита от клиппинга)."; },
      didCents: function (c) { return "Максимальная остаточная расстройка тона из-за точной подгонки циклов: " + c + " цента (практически не слышна)."; },
      didSm24: "В банке были 24-битные расширения (sm24): они отброшены, выход 16 бит.",
      didKept: "Пресеты, инструменты, диапазоны нот и скорости, корневые ноты, огибающие и режимы циклов не менялись.",
      warnTitle: "Предупреждения",
      warn: {
        len_diff: function (w) { return "длины L/R различаются (" + w.l + " и " + w.r + "), обрезано до " + w.m; },
        loop_diff: function () { return "петли L/R различаются, взяты петли левого канала"; },
        rate_diff: function () { return "частоты L/R различаются, взята частота левого"; },
        gain_clamped: function (w) { return "нормировка громкости " + w.factor.toFixed(2) + " вне диапазона [0,25; 2,0], ограничена"; }
      },
      table: "Подробности по сэмплам",
      cols: ["Сэмпл", "Было, отсч.", "Стало, отсч.", "Цикл был", "Цикл стал", "Расстройка, цент", "Сведение"],
      mixNone: "без выравнивания", mixBody: "тело", mixLoop: "цикл", mixLag: "лаг", mixMono: "моно",
      mixStereo: "стерео",
      errTitle: "Не получилось",
      errors: {
        not_sf2: "Это не файл SoundFont2: в начале нет заголовка RIFF/sfbk. Выберите файл с расширением .sf2.",
        chunk_corrupt: function (a) { return "Файл повреждён: блок «" + a.id + "» выходит за границы файла."; },
        no_chunk: function (a) { return "Файл неполный: нет блока " + a.name + "."; },
        no_smpl: "В файле нет аудиоданных (блок smpl).",
        rom_sample: function (a) { return "Сэмпл №" + a.index + " («" + a.name + "») — ROM-сэмпл. Такие банки не поддерживаются."; },
        compressed: "Это сжатый банк (SF3, Ogg Vorbis). Инструмент работает только с несжатым SF2.",
        bad_range: function (a) { return "Сэмпл №" + a.index + " («" + a.name + "») указывает за границы аудиоданных, файл повреждён."; },
        addr_offset: function (a) {
          return "Банк использует смещения адресов сэмпла в зонах (генератор «" + genName(a.gen) + "», зона №" + a.zone + "). " +
            "После передискретизации эти смещения теряют смысл, поэтому такие банки пока не поддерживаются. " +
            "Уменьшить его можно в Polyphone: удалите лишние пресеты и инструменты.";
        },
        bad_sample_ref: function (a) { return "Зона №" + a.zone + " ссылается на несуществующий сэмпл " + a.id + ", файл повреждён."; },
        no_inst: "В банке нет инструментов.",
        invalid_output: function (a) { return "Результат не прошёл самопроверку (" + a.detail + "). Исходный банк не изменён; пришлите его в Issues."; },
        big_endian: "Этот браузер на платформе big-endian не поддерживается.",
        too_big: function (a) { return "Файл слишком большой для обработки в браузере (" + a.mb + " МБ, максимум 1536 МБ). Воспользуйтесь скриптом SoundFonts/sf2_shrink.py."; },
        no_worker: "Не удалось запустить фоновый поток. Откройте страницу по адресу http(s), а не как локальный файл (file://); например, командой «python3 -m http.server» в папке site.",
        load_failed: "Не удалось загрузить модуль обработки (скорее всего, проблемы с сетью). Обновите страницу и попробуйте ещё раз.",
        oom: "Не хватило памяти браузера. Закройте лишние вкладки или воспользуйтесь скриптом SoundFonts/sf2_shrink.py.",
        internal: function (a) { return "Внутренняя ошибка: " + a.message; }
      }
    },
    en: {
      pick: "Drop an .sf2 file here or click to choose",
      picked: "Selected file:",
      change: "Choose another file",
      mb: "MB",
      bytes: "bytes",
      working: "Working…",
      go: "Shrink",
      cancel: "Cancel",
      cancelled: "Cancelled.",
      progress: function (a) {
        return (a.passes > 1 ? "Pass " + a.pass + " of " + a.passes + " · " : "") + a.rate + " Hz · sample " +
          Math.min(a.done + 1, a.total) + " of " + a.total + (a.name ? " (“" + a.name + "”)" : "");
      },
      fit: "Fits the firmware limit",
      nofit: "Does not fit the firmware limit",
      sizes: function (a) {
        var t = "Was " + a.before + " MB, now " + a.after + " MB";
        if (a.ratio >= 1.02) t += " (" + a.ratioText + "× smaller)";
        else if (a.ratio <= 0.98) t += " (larger)";
        else t += " (about the same size)";
        return t;
      },
      fileSize: function (n) { return "File size: " + n + " bytes."; },
      limit: "PurrMIDI limit: 8,384,512 bytes (≈ 8 MB).",
      download: "Download SNDFNT.SF2",
      downloadAs: function (n) { return "or save as " + n; },
      nextFit: "Put SNDFNT.SF2 in the root of the microSD card (FAT32), insert the card and power on the device.",
      nextNoFit: function (over) {
        return "The file is still " + over + " MB over the limit. You can delete unneeded presets and instruments in Polyphone " +
          "and run the bank through this tool again, or pick a smaller bank.";
      },
      alreadyFit: "The original file already fitted the limit, so shrinking was optional.",
      tried: "Sample rates tried:",
      did: "What was done",
      didRate: function (r) { return "Sample rate: " + r + " Hz (samples already at a lower rate were left as they are)."; },
      didPairs: function (n, z) {
        return n ? "Stereo pairs mixed to mono: " + n + "; duplicate right-channel zones removed: " + z + "." :
          "The bank has no stereo pairs, so no mono mixdown was needed.";
      },
      didGuard: function (g) { return "The mixed peak exceeded 16 bits; mixed samples were attenuated by " + g + " (clipping guard)."; },
      didCents: function (c) { return "Largest leftover pitch offset from exact loop fitting: " + c + " cents (practically inaudible)."; },
      didSm24: "The bank had 24-bit extensions (sm24): they were dropped, output is 16-bit.",
      didKept: "Presets, instruments, key and velocity ranges, root keys, envelopes and loop modes are unchanged.",
      warnTitle: "Warnings",
      warn: {
        len_diff: function (w) { return "L/R lengths differ (" + w.l + " and " + w.r + "), trimmed to " + w.m; },
        loop_diff: function () { return "L/R loops differ, the left channel loops were used"; },
        rate_diff: function () { return "L/R sample rates differ, the left rate was used"; },
        gain_clamped: function (w) { return "loudness normalisation " + w.factor.toFixed(2) + " is outside [0.25, 2.0], clamped"; }
      },
      table: "Per-sample details",
      cols: ["Sample", "Was, pts", "Now, pts", "Loop was", "Loop now", "Detune, cents", "Mixdown"],
      mixNone: "no alignment", mixBody: "body", mixLoop: "loop", mixLag: "lag", mixMono: "mono",
      mixStereo: "stereo",
      errTitle: "Could not shrink this file",
      errors: {
        not_sf2: "This is not a SoundFont2 file: there is no RIFF/sfbk header. Choose a file with the .sf2 extension.",
        chunk_corrupt: function (a) { return "The file is damaged: chunk “" + a.id + "” runs past the end of the file."; },
        no_chunk: function (a) { return "The file is incomplete: chunk " + a.name + " is missing."; },
        no_smpl: "The file has no audio data (smpl chunk).",
        rom_sample: function (a) { return "Sample #" + a.index + " (“" + a.name + "”) is a ROM sample. Such banks are not supported."; },
        compressed: "This is a compressed bank (SF3, Ogg Vorbis). The tool only works with uncompressed SF2.",
        bad_range: function (a) { return "Sample #" + a.index + " (“" + a.name + "”) points outside the audio data; the file is damaged."; },
        addr_offset: function (a) {
          return "The bank uses sample address offsets in zones (generator “" + genName(a.gen) + "”, zone #" + a.zone + "). " +
            "They lose their meaning after resampling, so such banks are not supported yet. " +
            "You can shrink it in Polyphone by deleting unneeded presets and instruments.";
        },
        bad_sample_ref: function (a) { return "Zone #" + a.zone + " refers to a non-existent sample " + a.id + "; the file is damaged."; },
        no_inst: "The bank has no instruments.",
        invalid_output: function (a) { return "The result failed the self-check (" + a.detail + "). Your original is untouched; please report this in Issues."; },
        big_endian: "Big-endian platforms are not supported.",
        too_big: function (a) { return "The file is too large to process in a browser (" + a.mb + " MB, max 1536 MB). Use the SoundFonts/sf2_shrink.py script instead."; },
        no_worker: "Could not start the background worker. Open the page over http(s) rather than as a local file (file://), e.g. run “python3 -m http.server” in the site folder.",
        load_failed: "Could not load the processing module (probably a network problem). Reload the page and try again.",
        oom: "The browser ran out of memory. Close other tabs or use the SoundFonts/sf2_shrink.py script.",
        internal: function (a) { return "Internal error: " + a.message; }
      }
    }
  }[LANG];

  var GEN_NAMES = {
    0: "startAddrsOffset", 1: "endAddrsOffset", 2: "startloopAddrsOffset", 3: "endloopAddrsOffset",
    4: "startAddrsCoarseOffset", 12: "endAddrsCoarseOffset", 45: "startloopAddrsCoarseOffset", 50: "endloopAddrsCoarseOffset"
  };
  function genName(n) { return GEN_NAMES[n] || String(n); }

  // ------------------------------------------------------------------------------------------
  var $ = function (id) { return document.getElementById(id); };
  var nf2 = new Intl.NumberFormat(LANG, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  var nf3 = new Intl.NumberFormat(LANG, { minimumFractionDigits: 3, maximumFractionDigits: 3 });
  var nf0 = new Intl.NumberFormat(LANG);
  function mb(bytes) { return nf2.format(bytes / 1048576); }

  function el(tag, attrs, kids) {
    var e = document.createElement(tag);
    if (attrs) Object.keys(attrs).forEach(function (k) {
      if (k === "text") e.textContent = attrs[k];
      else if (k === "class") e.className = attrs[k];
      else e.setAttribute(k, attrs[k]);
    });
    (kids || []).forEach(function (c) { if (c) e.appendChild(typeof c === "string" ? document.createTextNode(c) : c); });
    return e;
  }

  var state = { file: null, worker: null, url: null };
  var ui = {
    drop: $("drop"), input: $("file"), dropText: $("drop-text"), go: $("go"), cancel: $("cancel"),
    rate: $("rate"), gain: $("gain"), align: $("align"), maxlag: $("maxlag"),
    progress: $("progress"), bar: $("bar"), status: $("status"), out: $("result"), err: $("error")
  };

  function setFile(f) {
    state.file = f;
    clearOutput();
    ui.dropText.textContent = f ? T.picked + " " + f.name + " — " + mb(f.size) + " " + T.mb + ". " + T.change : T.pick;
    ui.go.disabled = !f;
  }

  function clearOutput() {
    ui.out.hidden = true; ui.out.textContent = "";
    ui.err.hidden = true; ui.err.textContent = "";
    if (state.url) { URL.revokeObjectURL(state.url); state.url = null; }
  }

  function showError(code, args) {
    ui.progress.hidden = true;
    ui.out.hidden = true;
    var m = T.errors[code] || T.errors.internal;
    var text = typeof m === "function" ? m(args || {}) : m;
    ui.err.textContent = "";
    ui.err.appendChild(el("strong", { text: T.errTitle + ". " }));
    ui.err.appendChild(document.createTextNode(text));
    ui.err.hidden = false;
  }

  function setBusy(busy) {
    ui.go.hidden = busy; ui.cancel.hidden = !busy;
    ui.input.disabled = busy;
    ui.drop.classList.toggle("busy", busy);
    ui.progress.hidden = !busy;
    if (!busy) ui.bar.removeAttribute("value");
  }

  function stopWorker() {
    if (state.worker) { state.worker.terminate(); state.worker = null; }
  }

  // ------------------------------------------------------------------------------------------
  function run() {
    if (!state.file) return;
    clearOutput();
    if (state.file.size > MAX_INPUT) return showError("too_big", { mb: nf0.format(Math.round(state.file.size / 1048576)) });

    var worker;
    try { worker = new Worker(WORKER_URL); } catch (e) { return showError("no_worker"); }
    state.worker = worker;
    setBusy(true);
    ui.status.textContent = T.working;

    var mode = ui.rate.value;
    var opts = {
      gainDb: parseFloat(ui.gain.value) || 0,
      align: ui.align.checked,
      maxlag: Math.max(1, Math.min(128, parseInt(ui.maxlag.value, 10) || 64))
    };
    var file = state.file;

    // После «Отмены» (terminate) браузер может ещё доставить событие от старого воркера, например
    // error оборванной загрузки скрипта: такие события нужно игнорировать.
    worker.onmessage = function (e) {
      if (state.worker !== worker) return;
      var m = e.data;
      if (m.type === "progress") {
        ui.bar.max = Math.max(1, m.total);
        ui.bar.value = m.done;
        ui.status.textContent = T.progress(Object.assign({}, m, { rate: nf0.format(m.rate) }));
      } else if (m.type === "done") {
        stopWorker(); setBusy(false);
        showResult(file, m);
      } else if (m.type === "error") {
        stopWorker(); setBusy(false);
        showError(m.code, Object.assign({ message: m.message }, m.args));
      }
    };
    worker.onerror = function (e) {
      if (state.worker !== worker) return;
      stopWorker(); setBusy(false);
      var msg = e.message || "worker error";
      showError(/memory|allocation/i.test(msg) ? "oom" : /importScripts|failed to load|NetworkError/i.test(msg) ? "load_failed" : "internal", { message: msg });
    };

    file.arrayBuffer().then(function (buffer) {
      if (state.worker !== worker) return;                // пока читали, нажали «Отмена»
      worker.postMessage({ buffer: buffer, mode: mode, opts: opts }, [buffer]);
    }, function () {
      stopWorker(); setBusy(false); showError("internal", { message: "cannot read file" });
    });
  }

  function cancel() {
    stopWorker(); setBusy(false);
    ui.err.textContent = T.cancelled; ui.err.hidden = false;
  }

  // ------------------------------------------------------------------------------------------
  function fmtLag(b) {
    return T.mixLag + " " + (b.lag >= 0 ? "+" : "") + b.lag + " (" + (b.sign < 0 ? "−" : "+") + Math.abs(b.corr).toFixed(2).replace(".", LANG === "ru" ? "," : ".") + ")";
  }
  function mixText(s) {
    var m = s.mix;
    if (!m) return T.mixMono;
    var f = " · ×" + nf3.format(m.factor);
    if (m.kind === "none") return T.mixNone + f;
    if (m.kind === "whole") return fmtLag(m.whole) + f;
    return T.mixBody + ": " + fmtLag(m.body) + ", " + T.mixLoop + ": " + fmtLag(m.loop) + f;
  }
  function loopText(l) { return l ? l[0] + ".." + l[1] : "—"; }

  function showResult(file, m) {
    var st = m.stats, fits = st.outBytes <= DEVICE_LIMIT;
    var blob = new Blob([m.buffer], { type: "application/octet-stream" });
    if (state.url) URL.revokeObjectURL(state.url);
    state.url = URL.createObjectURL(blob);

    var ratio = st.inBytes / st.outBytes;
    var root = ui.out;
    root.textContent = "";
    root.className = "result " + (fits ? "fit" : "nofit");

    root.appendChild(el("h3", { text: (fits ? "✓ " : "✕ ") + (fits ? T.fit : T.nofit) }));
    root.appendChild(el("p", { class: "sizes", text: T.sizes({ before: mb(st.inBytes), after: mb(st.outBytes), ratio: ratio, ratioText: nf2.format(ratio) }) }));
    root.appendChild(el("p", { class: "small", text: T.fileSize(nf0.format(st.outBytes)) + " " + T.limit }));

    var base = file.name.replace(/\.sf2$/i, "");
    var dl = el("a", { class: "btn", href: state.url, download: "SNDFNT.SF2", text: T.download });
    var alt = el("a", { class: "small alt-dl", href: state.url, download: base + "_small.sf2", text: T.downloadAs(base + "_small.sf2") });
    root.appendChild(el("div", { class: "dl" }, [dl, alt]));

    root.appendChild(el("p", { class: "next", text: fits ? T.nextFit : T.nextNoFit(mb(st.outBytes - DEVICE_LIMIT)) }));
    if (st.inBytes <= DEVICE_LIMIT) root.appendChild(el("p", { class: "small", text: T.alreadyFit }));
    if (m.tried.length > 1) {
      root.appendChild(el("p", { class: "small", text: T.tried + " " + m.tried.map(function (t) {
        return nf0.format(t.rate) + " → " + mb(t.bytes) + " " + T.mb;
      }).join("; ") + "." }));
    }

    var did = el("ul", null, [
      el("li", { text: T.didRate(nf0.format(st.rate)) }),
      el("li", { text: T.didPairs(st.merged, st.zonesRemoved) }),
      st.guard !== 1 ? el("li", { text: T.didGuard(nf3.format(st.guard)) }) : null,
      el("li", { text: T.didCents(nf3.format(st.worstCents)) }),
      st.sm24 ? el("li", { text: T.didSm24 }) : null,
      el("li", { text: T.didKept })
    ]);
    root.appendChild(el("details", { open: "" }, [el("summary", { text: T.did }), did]));

    if (st.warnings.length) {
      root.appendChild(el("details", { open: "" }, [
        el("summary", { text: T.warnTitle + " (" + st.warnings.length + ")" }),
        el("ul", null, st.warnings.map(function (w) {
          return el("li", { text: "«" + w.sample + "»: " + T.warn[w.code](w) });
        }))
      ]));
    }

    var head = el("tr", null, T.cols.map(function (c) { return el("th", { text: c }); }));
    var rows = st.samples.map(function (s) {
      return el("tr", null, [
        el("td", { text: s.name }), el("td", { text: nf0.format(s.nSrc) }), el("td", { text: nf0.format(s.nNew) }),
        el("td", { text: loopText(s.loopOld) }), el("td", { text: loopText(s.loopNew) }),
        el("td", { text: (s.cents >= 0 ? "+" : "−") + nf3.format(Math.abs(s.cents)) }), el("td", { text: mixText(s) })
      ]);
    });
    root.appendChild(el("details", null, [
      el("summary", { text: T.table + " (" + st.samples.length + ")" }),
      el("div", { class: "scroll tall" }, [el("table", null, [el("thead", null, [head]), el("tbody", null, rows)])])
    ]));

    root.hidden = false;
    root.scrollIntoView({ behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth", block: "nearest" });
  }

  // ------------------------------------------------------------------------------------------
  ui.input.addEventListener("change", function () { setFile(ui.input.files[0] || null); });
  ["dragenter", "dragover"].forEach(function (n) {
    ui.drop.addEventListener(n, function (e) { e.preventDefault(); ui.drop.classList.add("over"); });
  });
  ["dragleave", "drop"].forEach(function (n) {
    ui.drop.addEventListener(n, function (e) { e.preventDefault(); ui.drop.classList.remove("over"); });
  });
  ui.drop.addEventListener("drop", function (e) {
    if (ui.input.disabled) return;
    var f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (f) setFile(f);
  });
  ui.go.addEventListener("click", run);
  ui.cancel.addEventListener("click", cancel);
  setFile(null);
})();
