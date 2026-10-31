// Веб-демо PurrMIDI: звуковые движки прошивки (WebAssembly) в AudioWorklet + экранная клавиатура,
// клавиши компьютера и Web MIDI. Страницы: demo/index.html (en, по умолчанию) и ru/demo/index.html (ru).
(function () {
  "use strict";

  var LANG = document.documentElement.lang === "en" ? "en" : "ru";
  var SCRIPT_URL = document.currentScript.src;
  function asset(path) { return new URL(path, SCRIPT_URL).href; }
  function $(id) { return document.getElementById(id); }

  // ---------------------------------------------------------------------------------------------------------
  // Тексты
  // ---------------------------------------------------------------------------------------------------------
  var TXT = {
    ru: {
      start: "Включить звук",
      starting: "Загружаю движки…",
      on: "Звук включён",
      statusOff: "Звук выключен. Нажмите кнопку или любую клавишу.",
      statusReady: "Играйте: мышью, пальцем или клавишами компьютера.",
      noAudio: "Этот браузер не поддерживает AudioWorklet. Откройте страницу в современном Chrome, Edge, Firefox или Safari.",
      loadFail: "Не удалось запустить звук: ",
      noNotes: "нет нот",
      instr: "Инструмент",
      knobs: "Ручки инструмента",
      knobsNone: "У этого инструмента нет ручек, кроме колеса высоты тона.",
      reset: "Вернуть по умолчанию",
      pc: "Program Change ",
      bend: "Колесо высоты тона",
      bendNo: "Этот инструмент не реагирует на колесо высоты тона.",
      aftertouch: "Нажим (aftertouch)",
      chorus: "Хорус",
      drawbars: "Регистры (drawbars)",
      bankChoose: "Банк",
      bankLoad: "Загрузить банк",
      bankOwn: "или свой файл .sf2",
      bankLoading: "Загружаю: ",
      bankParsing: "Разбираю банк…",
      bankDone: "Банк загружен: ",
      bankPresets: ", пресетов: ",
      bankFail: "Не удалось прочитать банк: файл повреждён или это не SoundFont2.",
      bankHttp: "Не удалось скачать банк. Проверьте соединение и попробуйте ещё раз.",
      bankBig: "Файл слишком большой для браузерного демо (больше 120 МБ).",
      bankOver: "Банк больше 8 384 512 байт: на плату он не поместится. Уменьшите его инструментом на странице «Уменьшить SoundFont».",
      bankNeed: "Чтобы услышать SoundFont, загрузите банк.",
      preset: "Пресет",
      oct: "Октава",
      sustain: "Сустейн",
      midiBtn: "Подключить MIDI-клавиатуру",
      midiNo: "Этот браузер не поддерживает Web MIDI. Подойдёт Chrome или Edge.",
      midiDenied: "Доступ к MIDI не разрешён.",
      midiNone: "MIDI-клавиатуры не найдено. Подключите её: список обновится сам.",
      midiOk: "MIDI подключён: ",
      sizeMb: " МБ"
    },
    en: {
      start: "Turn sound on",
      starting: "Loading engines…",
      on: "Sound is on",
      statusOff: "Sound is off. Press the button or any key.",
      statusReady: "Play with the mouse, your finger or the computer keyboard.",
      noAudio: "This browser does not support AudioWorklet. Open the page in a current Chrome, Edge, Firefox or Safari.",
      loadFail: "Could not start the sound: ",
      noNotes: "no notes",
      instr: "Instrument",
      knobs: "Instrument knobs",
      knobsNone: "This instrument has no knobs except the pitch wheel.",
      reset: "Reset to defaults",
      pc: "Program Change ",
      bend: "Pitch wheel",
      bendNo: "This instrument ignores the pitch wheel.",
      aftertouch: "Pressure (aftertouch)",
      chorus: "Chorus",
      drawbars: "Drawbars",
      bankChoose: "Bank",
      bankLoad: "Load bank",
      bankOwn: "or your own .sf2 file",
      bankLoading: "Loading: ",
      bankParsing: "Parsing the bank…",
      bankDone: "Bank loaded: ",
      bankPresets: ", presets: ",
      bankFail: "Could not read the bank: the file is damaged or is not a SoundFont2.",
      bankHttp: "Could not download the bank. Check your connection and try again.",
      bankBig: "The file is too large for the browser demo (over 120 MB).",
      bankOver: "The bank is over 8,384,512 bytes, so it will not fit on the board. Shrink it with the “Shrink a SoundFont” tool.",
      bankNeed: "Load a bank to hear the SoundFont engine.",
      preset: "Preset",
      oct: "Octave",
      sustain: "Sustain",
      midiBtn: "Connect a MIDI keyboard",
      midiNo: "This browser does not support Web MIDI. Try Chrome or Edge.",
      midiDenied: "MIDI access was not granted.",
      midiNone: "No MIDI keyboard found. Plug one in: the list updates by itself.",
      midiOk: "MIDI connected: ",
      sizeMb: " MB"
    }
  };
  var T = TXT[LANG];

  // ---------------------------------------------------------------------------------------------------------
  // Инструменты и их ручки (значения по умолчанию совпадают с теми, что движок ставит в Init())
  // ---------------------------------------------------------------------------------------------------------
  function K(cc, ru, en, def, extra) {
    var o = { cc: cc, name: LANG === "en" ? en : ru, def: def };
    if (extra) for (var k in extra) o[k] = extra[k];
    return o;
  }
  var INSTR = [
    {
      name: "E-Piano", bend: true,
      desc: LANG === "en" ? "FM electric piano in the Rhodes style. Plays by default." : "FM-электропиано в духе Rhodes. Звучит по умолчанию.",
      knobs: [K(74, "Яркость", "Brightness", 64), K(72, "Отпускание", "Release", 41),
              K(1, "Мод-колесо (добавляет яркость)", "Mod wheel (adds brightness)", 0)]
    },
    {
      name: "Pluck", bend: true,
      desc: LANG === "en" ? "Nylon-string guitar." : "Гитара с нейлоновыми струнами.",
      knobs: [K(74, "Яркость", "Brightness", 42), K(72, "Длительность затухания", "Decay", 97),
              K(1, "Мод-колесо (добавляет яркость)", "Mod wheel (adds brightness)", 0)]
    },
    {
      name: "Lead", bend: true,
      desc: LANG === "en" ? "Analog-style lead synth." : "Аналоговый синтезатор-лид.",
      knobs: [K(74, "Срез фильтра", "Filter cutoff", 32), K(71, "Резонанс", "Resonance", 42),
              K(1, "Мод-колесо (глубже огибающая фильтра)", "Mod wheel (deeper filter envelope)", 0),
              K(73, "Атака", "Attack", 0), K(72, "Отпускание", "Release", 15)]
    },
    {
      name: "Organ", bend: true,
      desc: LANG === "en" ? "Tonewheel organ." : "Тонколесный орган.",
      groups: true,
      knobs: [
        K(20, "16′", "16′", 127, { group: "drawbars" }), K(21, "5⅓′", "5⅓′", 127, { group: "drawbars" }),
        K(22, "8′", "8′", 127, { group: "drawbars" }), K(23, "4′", "4′", 127, { group: "drawbars" }),
        K(24, "2⅔′", "2⅔′", 127, { group: "drawbars" }), K(25, "2′", "2′", 127, { group: "drawbars" }),
        K(26, "1⅗′", "1⅗′", 127, { group: "drawbars" }), K(27, "1⅓′", "1⅓′", 127, { group: "drawbars" }),
        K(28, "1′", "1′", 127, { group: "drawbars" }),
        K(70, "Хорус", "Chorus", 0, { toggle: true }),
        K(1, "Мод-колесо (≥ 64 включает хорус)", "Mod wheel (64 and up turns the chorus on)", 0),
        K(71, "Скорость хоруса", "Chorus speed", 8), K(72, "Глубина хоруса", "Chorus depth", 127),
        K(77, "Атака", "Attack", 20), K(78, "Отпускание", "Release", 20)
      ]
    },
    {
      name: "Violin", bend: true, aftertouch: true,
      desc: LANG === "en" ? "Violin: a physical model of a bowed string." : "Скрипка: физическая модель смычковой струны.",
      knobs: [K(1, "Мод-колесо (добавляет вибрато)", "Mod wheel (adds vibrato)", 0), K(70, "Положение смычка", "Bow position", 48),
              K(71, "Прижим смычка", "Bow pressure", 70), K(75, "Шум смычка", "Bow noise", 51),
              K(74, "Яркость", "Brightness", 70), K(73, "Атака", "Attack", 64), K(72, "Отпускание", "Release", 64)]
    },
    {
      name: "SoundFont", bend: true, sf: true,
      desc: LANG === "en" ? "Any SoundFont2 bank: every preset is a separate instrument." : "Любой банк SoundFont2: каждый пресет банка — отдельный инструмент.",
      knobs: [K(1, "Мод-колесо (вибрато)", "Mod wheel (vibrato)", 0)]
    }
  ];

  var BANKS = [
    { file: "000_Florestan_Piano.sf2", size: 7809600, ru: "Florestan Piano (как в готовой прошивке)", en: "Florestan Piano (as in the ready firmware)" },
    { file: "040_Florestan_String_Quartet.sf2", size: 3176318, ru: "Florestan String Quartet", en: "Florestan String Quartet" },
    { file: "068_Florestan_Woodwinds.sf2", size: 1592486, ru: "Florestan Woodwinds", en: "Florestan Woodwinds" },
    { file: "052_Florestan_Ahh_Choir.sf2", size: 785510, ru: "Florestan Ahh Choir", en: "Florestan Ahh Choir" },
    { file: "014_Florestan_Tubular_Bells_and_glide.sf2", size: 577318, ru: "Florestan Tubular Bells", en: "Florestan Tubular Bells" }
  ];
  var BOARD_SF2_LIMIT = 8384512;
  var BROWSER_SF2_LIMIT = 120 * 1024 * 1024;

  // Клавиши компьютера: физические позиции (event.code), поэтому работает на любой раскладке.
  var KEYMAP = {
    KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11,
    KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16, Quote: 17
  };

  var NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  function noteName(n) { return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1); }
  function isBlack(n) { return [1, 3, 6, 8, 10].indexOf(n % 12) >= 0; }

  // ---------------------------------------------------------------------------------------------------------
  // Состояние
  // ---------------------------------------------------------------------------------------------------------
  var ctx = null, node = null, startPromise = null;
  var wasmPromise = null;
  var uiEngine = 0;                 // выбранная карточка 0..5
  var presets = [];                 // имена пресетов загруженного банка
  var bankId = 0, bankWaiters = {};
  var downCount = {};               // нота -> число источников, удерживающих её
  var held = {};                    // нота -> true (для экрана)
  var startNote = 48;
  var sustainLatched = false, sustainKey = false, sustainMidi = false, sustainSent = 0;
  var pending = [];                 // нажатия до запуска звука

  // ---------------------------------------------------------------------------------------------------------
  // Связь с воркле́том
  // ---------------------------------------------------------------------------------------------------------
  function send(m, transfer) {
    if (node) node.port.postMessage(m, transfer || []);
  }

  function prefetchWasm() {
    if (!wasmPromise) {
      wasmPromise = fetch(asset("purrmidi.wasm")).then(function (r) {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.arrayBuffer();
      });
      wasmPromise.catch(function () { /* ошибку покажет start() */ });
    }
    return wasmPromise;
  }

  function setDot(kind) { $("dot").className = "dot " + kind; }
  function setStatus(text) { $("status").textContent = text; }

  function start() {
    if (startPromise) {
      if (ctx && ctx.state === "suspended") ctx.resume();
      return startPromise;
    }
    if (!window.AudioContext && !window.webkitAudioContext || !window.AudioWorkletNode) {
      setStatus(T.noAudio);
      return Promise.reject(new Error("no audio"));
    }
    $("start").disabled = true;
    $("start").textContent = T.starting;
    setDot("wait");
    startPromise = (async function () {
      var AC = window.AudioContext || window.webkitAudioContext;
      try { ctx = new AC({ sampleRate: 48000, latencyHint: "interactive" }); }
      catch (e) { ctx = new AC({ latencyHint: "interactive" }); }
      var wasm = await prefetchWasm();
      await ctx.audioWorklet.addModule(asset("worklet.js"));
      node = new AudioWorkletNode(ctx, "purrmidi", {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { wasm: wasm.slice(0) }
      });
      await new Promise(function (resolve, reject) {
        node.port.onmessage = function (e) {
          if (e.data.t === "ready") { node.port.onmessage = onWorkletMessage; resolve(); }
          else if (e.data.t === "error") reject(new Error(e.data.message));
        };
        setTimeout(function () { reject(new Error("timeout")); }, 15000);
      });
      node.connect(ctx.destination);
      if (ctx.state === "suspended") await ctx.resume();
      var vol = +$("vol").value;
      send({ t: "cc", c: 7, v: vol });
      setDot("ok");
      $("start").textContent = T.on;
      $("start").classList.add("live");
      setStatus(T.statusReady);
      pending.splice(0).forEach(function (p) { if (down(p.note)) send({ t: "on", n: p.note, v: p.vel }); });
      sendSustain();
    })();
    startPromise.catch(function (err) {
      setDot("off");
      $("start").disabled = false;
      $("start").classList.remove("live");
      $("start").textContent = T.start;
      setStatus(T.loadFail + (err && err.message || err));
      startPromise = null;
      if (ctx) { try { ctx.close(); } catch (e) { /* уже закрыт */ } ctx = null; }
      node = null;
    });
    return startPromise;
  }

  function onWorkletMessage(e) {
    var m = e.data;
    if (m.t === "state") onState(m);
    else if (m.t === "chord") $("scr-chord").textContent = m.text;
    else if (m.t === "bank") {
      var w = bankWaiters[m.id];
      if (w) { delete bankWaiters[m.id]; w(m.presets); }
    }
  }

  function onState(m) {
    var eng = Math.min(m.program, 5);
    var pad = String(m.program + 1);
    if (pad.length < 2) pad = "0" + pad;
    $("scr-prog").textContent = pad + " " + m.name;
    if (eng !== uiEngine) { setEngine(eng, false); }
    if (m.program >= 5 && presets.length) $("preset-sel").value = String(m.program - 5);
  }

  // ---------------------------------------------------------------------------------------------------------
  // Выбор инструмента, ручки
  // ---------------------------------------------------------------------------------------------------------
  function buildInstrButtons() {
    var box = $("instr");
    INSTR.forEach(function (it, i) {
      var b = document.createElement("button");
      b.type = "button";
      b.className = "instr-btn c" + (i % 4);
      b.setAttribute("aria-pressed", "false");
      b.dataset.i = String(i);
      b.innerHTML = "<strong></strong><small></small>";
      b.firstChild.textContent = it.name;
      b.lastChild.textContent = T.pc + (i < 5 ? i : "5…");
      b.addEventListener("click", function () { pickEngine(i); });
      box.appendChild(b);
    });
  }

  function pickEngine(i) {
    setEngine(i, true);
    var p = (i === 5 && presets.length) ? 5 + (+$("preset-sel").value || 0) : i;
    if (node) send({ t: "prog", p: p });
    else start().then(function () { send({ t: "prog", p: p }); }).catch(function () {});
  }

  function setEngine(i, byUser) {
    uiEngine = i;
    var btns = $("instr").children;
    for (var k = 0; k < btns.length; k++) btns[k].setAttribute("aria-pressed", k === i ? "true" : "false");
    var it = INSTR[i];
    $("instr-desc").textContent = it.desc;
    $("sfbox").hidden = !it.sf;
    buildKnobs();
    var wheel = $("bend");
    wheel.disabled = !it.bend;
    $("bend-note").textContent = it.bend ? "" : T.bendNo;
    $("at-row").hidden = !it.aftertouch;
    if (!it.sf) $("sf-hint").textContent = "";
    else updateSfHint();
  }

  function sendCC(cc, v) { send({ t: "cc", c: cc, v: v }); }

  function buildKnobs() {
    var box = $("knobs");
    box.textContent = "";
    var it = INSTR[uiEngine];
    $("knobs-none").hidden = it.knobs.length > 0;
    $("knobs-reset").hidden = it.knobs.length === 0;
    var groupEl = null, lastGroup = null;
    it.knobs.forEach(function (kb) {
      var parent = box;
      if (kb.group) {
        if (lastGroup !== kb.group) {
          groupEl = document.createElement("div");
          groupEl.className = "knob-group";
          var h = document.createElement("div");
          h.className = "knob-group-title";
          h.textContent = T.drawbars;
          groupEl.appendChild(h);
          var grid = document.createElement("div");
          grid.className = "drawbars";
          groupEl.appendChild(grid);
          box.appendChild(groupEl);
          lastGroup = kb.group;
        }
        parent = groupEl.lastChild;
      }
      var label = document.createElement("label");
      label.className = kb.group ? "knob drawbar" : "knob";
      var name = document.createElement("span");
      name.className = "k-name";
      name.textContent = kb.name + " ";
      var tag = document.createElement("code");
      tag.textContent = "CC" + kb.cc;
      name.appendChild(tag);
      var input = document.createElement("input");
      input.dataset.cc = String(kb.cc);
      input.dataset.def = String(kb.def);
      var out = document.createElement("output");
      if (kb.toggle) {
        input.type = "checkbox";
        input.checked = kb.def >= 64;
        input.addEventListener("change", function () { sendCC(kb.cc, input.checked ? 127 : 0); });
        label.className = "knob toggle";
        label.appendChild(input);
        label.appendChild(name);
      } else {
        input.type = "range";
        input.min = "0"; input.max = "127"; input.value = String(kb.def);
        out.textContent = String(kb.def);
        input.addEventListener("input", function () { out.textContent = input.value; sendCC(kb.cc, +input.value); });
        if (kb.group) input.setAttribute("aria-label", kb.name + " CC" + kb.cc);
        label.appendChild(name);
        label.appendChild(input);
        label.appendChild(out);
      }
      parent.appendChild(label);
    });
  }

  function resetKnobs() {
    var inputs = $("knobs").querySelectorAll("input");
    for (var i = 0; i < inputs.length; i++) {
      var inp = inputs[i], def = +inp.dataset.def;
      if (inp.type === "checkbox") inp.checked = def >= 64; else {
        inp.value = String(def);
        var o = inp.parentNode.querySelector("output");
        if (o) o.textContent = String(def);
      }
      sendCC(+inp.dataset.cc, def);
    }
  }

  // CC с настоящей MIDI-клавиатуры двигает ползунки на экране
  function syncKnob(cc, v) {
    var inputs = $("knobs").querySelectorAll("input");
    for (var i = 0; i < inputs.length; i++) {
      var inp = inputs[i];
      if (+inp.dataset.cc !== cc) continue;
      if (inp.type === "checkbox") inp.checked = v > 63;
      else {
        inp.value = String(v);
        var o = inp.parentNode.querySelector("output");
        if (o) o.textContent = String(v);
      }
    }
    if (cc === 7) $("vol").value = String(v);
  }

  // ---------------------------------------------------------------------------------------------------------
  // SoundFont
  // ---------------------------------------------------------------------------------------------------------
  function fmtMb(bytes) {
    var v = (bytes / 1048576).toFixed(1);
    if (LANG === "ru") v = v.replace(".", ",");
    return v + T.sizeMb;
  }

  function buildBankSelect() {
    var sel = $("bank-sel");
    BANKS.forEach(function (b, i) {
      var o = document.createElement("option");
      o.value = String(i);
      o.textContent = (LANG === "en" ? b.en : b.ru) + ", " + fmtMb(b.size);
      sel.appendChild(o);
    });
  }

  function updateSfHint() {
    $("sf-hint").textContent = presets.length ? "" : T.bankNeed;
  }

  function loadBankBuffer(buf, label, sizeBytes) {
    setStatusBank(T.bankParsing);
    var id = ++bankId;
    return new Promise(function (resolve) {
      bankWaiters[id] = resolve;
      send({ t: "bank", id: id, buf: buf }, [buf]);
    }).then(function (names) {
      presets = names;
      var sel = $("preset-sel");
      sel.textContent = "";
      names.forEach(function (n, i) {
        var o = document.createElement("option");
        o.value = String(i);
        o.textContent = (n || "—");
        sel.appendChild(o);
      });
      sel.disabled = names.length === 0;
      if (!names.length) { setStatusBank(T.bankFail, true); updateSfHint(); return; }
      var msg = T.bankDone + label + T.bankPresets + names.length;
      if (sizeBytes > BOARD_SF2_LIMIT) msg += ". " + T.bankOver;
      setStatusBank(msg);
      updateSfHint();
      send({ t: "prog", p: 5 });
    });
  }

  function setStatusBank(text, isError) {
    var el = $("bank-status");
    el.textContent = text;
    el.className = "hint" + (isError ? " err" : "");
  }

  async function fetchBank(url, expected, onProgress) {
    var res = await fetch(url);
    if (!res.ok) throw new Error("HTTP " + res.status);
    var total = +res.headers.get("Content-Length") || expected;
    if (!res.body || !res.body.getReader) return res.arrayBuffer();
    var reader = res.body.getReader(), chunks = [], got = 0;
    for (;;) {
      var r = await reader.read();
      if (r.done) break;
      chunks.push(r.value);
      got += r.value.length;
      onProgress(Math.min(1, got / total));
    }
    var out = new Uint8Array(got), o = 0;
    chunks.forEach(function (c) { out.set(c, o); o += c.length; });
    return out.buffer;
  }

  async function onLoadBank() {
    var b = BANKS[+$("bank-sel").value];
    var btn = $("bank-load");
    btn.disabled = true; $("bank-file").disabled = true;
    try {
      await start();
      var prog = $("bank-prog");
      prog.hidden = false; prog.value = 0;
      setStatusBank(T.bankLoading + (LANG === "en" ? b.en : b.ru) + "…");
      var buf = await fetchBank(asset("banks/" + b.file), b.size, function (f) { prog.value = f; });
      prog.hidden = true;
      await loadBankBuffer(buf, LANG === "en" ? b.en : b.ru, buf.byteLength);
    } catch (err) {
      $("bank-prog").hidden = true;
      setStatusBank(T.bankHttp, true);
    } finally {
      btn.disabled = false; $("bank-file").disabled = false;
    }
  }

  async function onOwnFile() {
    var f = $("bank-file").files[0];
    $("bank-file").value = "";
    if (!f) return;
    if (f.size > BROWSER_SF2_LIMIT) { setStatusBank(T.bankBig, true); return; }
    $("bank-load").disabled = true;
    try {
      await start();
      var buf = await f.arrayBuffer();
      await loadBankBuffer(buf, f.name, f.size);
    } catch (err) {
      setStatusBank(T.bankFail, true);
    } finally {
      $("bank-load").disabled = false;
    }
  }

  // ---------------------------------------------------------------------------------------------------------
  // Ноты
  // ---------------------------------------------------------------------------------------------------------
  function down(note) { return (downCount[note] || 0) > 0; }

  function noteOn(note, vel) {
    if (note < 0 || note > 127) return;
    downCount[note] = (downCount[note] || 0) + 1;
    held[note] = true;
    if (ctx && ctx.state === "suspended") ctx.resume();   // Safari гасит контекст при смене вкладки
    if (node) send({ t: "on", n: note, v: vel });
    else { pending.push({ note: note, vel: vel }); start().catch(function () {}); }
    paintKey(note, true);
    paintScreen();
  }

  function noteOff(note) {
    if (!downCount[note]) return;
    downCount[note]--;
    if (downCount[note] > 0) return;
    delete downCount[note];
    delete held[note];
    send({ t: "off", n: note });
    paintKey(note, false);
    paintScreen();
  }

  function allUiOff() {
    Object.keys(downCount).forEach(function (n) { delete downCount[n]; paintKey(+n, false); });
    held = {};
    ptrNote = {};
    kbHeld = {};
    send({ t: "panic" });
    paintScreen();
    $("scr-chord").textContent = "";
  }

  function paintScreen() {
    var notes = Object.keys(held).map(Number).sort(function (a, b) { return a - b; });
    $("scr-notes").textContent = notes.length ? notes.map(noteName).join(" ") : T.noNotes;
    if (!notes.length) $("scr-chord").textContent = "";
  }

  function sendSustain() {
    var v = (sustainLatched || sustainKey || sustainMidi) ? 127 : 0;
    if (v !== sustainSent && node) { sustainSent = v; sendCC(64, v); }
    $("sustain").setAttribute("aria-pressed", sustainLatched ? "true" : "false");
    $("sustain").classList.toggle("lit", v === 127);
  }

  // ---------------------------------------------------------------------------------------------------------
  // Экранная клавиатура
  // ---------------------------------------------------------------------------------------------------------
  var keyEls = {};
  var ptrNote = {};      // pointerId -> нота
  var kbHeld = {};       // event.code -> нота

  function paintKey(note, on) {
    var el = keyEls[note];
    if (el) el.classList.toggle("on", on);
  }

  function buildPiano() {
    var piano = $("piano");
    var width = $("piano-scroll").clientWidth || 600;
    var whites = Math.max(8, Math.min(22, Math.floor(width / 38)));
    piano.textContent = "";
    keyEls = {};
    var list = [], n = startNote, w = 0;
    while (w < whites && n <= 127) { list.push(n); if (!isBlack(n)) w++; n++; }
    // закончить на белой клавише
    while (list.length && isBlack(list[list.length - 1])) list.pop();
    var whiteCount = list.filter(function (x) { return !isBlack(x); }).length;
    var step = 100 / whiteCount, wi = 0;
    list.forEach(function (note) {
      var el = document.createElement("div");
      el.dataset.note = String(note);
      if (isBlack(note)) {
        el.className = "key black";
        el.style.left = (wi * step - step * 0.3) + "%";
        el.style.width = (step * 0.6) + "%";
      } else {
        el.className = "key white";
        el.style.left = (wi * step) + "%";
        el.style.width = step + "%";
        if (note % 12 === 0) {
          var lab = document.createElement("span");
          lab.textContent = noteName(note);
          el.appendChild(lab);
        }
        wi++;
      }
      keyEls[note] = el;
    });
    // белые добавляются первыми, чтобы чёрные лежали поверх
    list.filter(function (x) { return !isBlack(x); }).forEach(function (x) { piano.appendChild(keyEls[x]); });
    list.filter(isBlack).forEach(function (x) { piano.appendChild(keyEls[x]); });
    Object.keys(held).forEach(function (x) { paintKey(+x, true); });
    $("oct-name").textContent = noteName(startNote);
  }

  function shiftOctave(d) {
    var s = startNote + 12 * d;
    if (s < 12 || s > 96) return;
    allUiOff();
    startNote = s;
    buildPiano();
  }

  function keyFromPoint(x, y) {
    var el = document.elementFromPoint(x, y);
    while (el && !(el.classList && el.classList.contains("key"))) el = el.parentNode;
    return el;
  }

  function pressAt(e) {
    var el = keyFromPoint(e.clientX, e.clientY);
    var id = e.pointerId, prev = ptrNote[id];
    if (!el) {
      if (prev !== undefined) { noteOff(prev); delete ptrNote[id]; }
      return;
    }
    var note = +el.dataset.note;
    if (prev === note) return;
    if (prev !== undefined) noteOff(prev);
    var r = el.getBoundingClientRect();
    var f = Math.max(0, Math.min(1, (e.clientY - r.top) / r.height));
    var vel = Math.round(45 + 82 * f);   // ближе к нижнему краю клавиши - громче
    ptrNote[id] = note;
    noteOn(note, vel);
  }

  function bindPiano() {
    var piano = $("piano");
    piano.addEventListener("pointerdown", function (e) {
      if (e.button > 0) return;
      e.preventDefault();
      try { piano.setPointerCapture(e.pointerId); } catch (err) { /* не критично */ }
      pressAt(e);
    });
    piano.addEventListener("pointermove", function (e) {
      if (ptrNote[e.pointerId] === undefined && e.buttons === 0) return;
      pressAt(e);
    });
    function up(e) {
      var n = ptrNote[e.pointerId];
      if (n !== undefined) { delete ptrNote[e.pointerId]; noteOff(n); }
    }
    piano.addEventListener("pointerup", up);
    piano.addEventListener("pointercancel", up);
    piano.addEventListener("contextmenu", function (e) { e.preventDefault(); });
  }

  function isTypingTarget(t) {
    if (!t || !t.tagName) return false;
    var tag = t.tagName;
    if (tag === "TEXTAREA" || tag === "SELECT" || tag === "SUMMARY") return true;
    if (tag === "INPUT") return t.type !== "range" && t.type !== "checkbox" ? true : false;
    return false;
  }

  function bindComputerKeyboard() {
    document.addEventListener("keydown", function (e) {
      if (e.ctrlKey || e.metaKey || e.altKey || isTypingTarget(e.target)) return;
      if (e.code === "Space") {
        e.preventDefault();
        if (!e.repeat) { sustainKey = true; sendSustain(); if (!node) start().catch(function () {}); }
        return;
      }
      if (e.repeat) return;
      if (e.code === "KeyZ") { shiftOctave(-1); return; }
      if (e.code === "KeyX") { shiftOctave(1); return; }
      var off = KEYMAP[e.code];
      if (off === undefined || kbHeld[e.code] !== undefined) return;
      var note = startNote + off;
      if (note > 127) return;
      e.preventDefault();
      kbHeld[e.code] = note;
      noteOn(note, 100);
    });
    document.addEventListener("keyup", function (e) {
      if (e.code === "Space") {
        if (isTypingTarget(e.target)) return;
        e.preventDefault();
        sustainKey = false; sendSustain();
        return;
      }
      var n = kbHeld[e.code];
      if (n !== undefined) { delete kbHeld[e.code]; noteOff(n); }
    });
    window.addEventListener("blur", function () { sustainKey = false; sendSustain(); allUiOff(); });
    document.addEventListener("visibilitychange", function () { if (document.hidden) allUiOff(); });
  }

  // ---------------------------------------------------------------------------------------------------------
  // Web MIDI
  // ---------------------------------------------------------------------------------------------------------
  var midiAccess = null;

  function onMidi(e) {
    var d = e.data;
    if (!d || d.length < 1 || d[0] >= 0xf0) return;
    var cmd = d[0] & 0xf0, a = d[1] | 0, b = d[2] | 0;
    if (!node) return;
    switch (cmd) {
      case 0x90:
        if (b > 0) noteOn(a, b); else noteOff(a);
        break;
      case 0x80: noteOff(a); break;
      case 0xb0:
        if (a === 64) { sustainMidi = b >= 64; sustainSent = -1; sendSustain(); break; }
        sendCC(a, b);
        syncKnob(a, b);
        break;
      case 0xc0: send({ t: "pc", p: a }); break;
      case 0xd0: send({ t: "at", v: a }); break;
      case 0xe0:
        send({ t: "pb", v: ((b << 7) | a) - 8192 });
        $("bend").value = String(((b << 7) | a) - 8192);
        break;
    }
  }

  function bindMidiInputs() {
    var names = [];
    midiAccess.inputs.forEach(function (inp) {
      inp.onmidimessage = onMidi;
      names.push(inp.name);
    });
    $("midi-status").textContent = names.length ? T.midiOk + names.join(", ") : T.midiNone;
  }

  async function connectMidi() {
    if (!navigator.requestMIDIAccess) { $("midi-status").textContent = T.midiNo; return; }
    start().catch(function () {});
    try {
      midiAccess = await navigator.requestMIDIAccess({ sysex: false });
      midiAccess.onstatechange = bindMidiInputs;
      bindMidiInputs();
    } catch (err) {
      $("midi-status").textContent = T.midiDenied;
    }
  }

  // ---------------------------------------------------------------------------------------------------------
  // Инициализация
  // ---------------------------------------------------------------------------------------------------------
  function init() {
    buildInstrButtons();
    buildBankSelect();
    setEngine(0, false);
    $("start").textContent = T.start;
    setStatus(T.statusOff);
    setDot("off");
    paintScreen();
    $("scr-prog").textContent = "01 E-Piano";

    $("start").addEventListener("click", function () { start().catch(function () {}); this.blur(); });
    $("vol").addEventListener("input", function () { sendCC(7, +this.value); });
    $("knobs-reset").addEventListener("click", resetKnobs);
    $("bank-load").addEventListener("click", onLoadBank);
    $("bank-file").addEventListener("change", onOwnFile);
    $("preset-sel").addEventListener("change", function () { send({ t: "prog", p: 5 + (+this.value) }); });
    $("oct-down").addEventListener("click", function () { shiftOctave(-1); });
    $("oct-up").addEventListener("click", function () { shiftOctave(1); });
    $("sustain").addEventListener("click", function () { sustainLatched = !sustainLatched; sendSustain(); });
    $("midi-btn").addEventListener("click", connectMidi);

    var bend = $("bend");
    bend.addEventListener("input", function () { send({ t: "pb", v: +bend.value }); });
    function springBack() { bend.value = "0"; send({ t: "pb", v: 0 }); }
    bend.addEventListener("pointerup", springBack);
    bend.addEventListener("pointercancel", springBack);
    bend.addEventListener("keyup", springBack);
    $("at").addEventListener("input", function () { send({ t: "at", v: +this.value }); });
    $("at").addEventListener("pointerup", function () { this.value = "0"; send({ t: "at", v: 0 }); });

    // для узких экранов начинаем с до первой октавы выше
    startNote = $("piano-scroll").clientWidth < 600 ? 60 : 48;
    buildPiano();
    bindPiano();
    bindComputerKeyboard();
    var rt, lastWidth = $("piano-scroll").clientWidth;
    window.addEventListener("resize", function () {
      clearTimeout(rt);
      rt = setTimeout(function () {
        var w = $("piano-scroll").clientWidth;
        if (w === lastWidth) return;      // на телефонах resize приходит и при скрытии адресной строки
        lastWidth = w;
        allUiOff();
        buildPiano();
      }, 150);
    });

    if (!navigator.requestMIDIAccess) $("midi-status").textContent = T.midiNo;
    // wasm маленький (около 140 КБ): качаем заранее, чтобы звук включался сразу
    prefetchWasm();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
