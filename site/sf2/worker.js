/*
 * worker.js — запускает sf2shrink.js вне основного потока, чтобы страница не зависала.
 *
 * Сообщения от страницы:  { buffer: ArrayBuffer, mode: "auto" | <частота Гц>, opts: {gainDb, align, maxlag} }
 * Ответы:                 { type: "progress", ... } | { type: "done", buffer, stats, tried } | { type: "error", code, args, message }
 */
importScripts("sf2shrink.js");

// В режиме «авто» пробуем частоты от рекомендованной вниз и берём первую, при которой банк влезает.
var AUTO_RATES = [24000, 22050, 16000];

self.onmessage = function (e) {
  var msg = e.data;
  try {
    var S = self.SF2Shrink;
    var rates = msg.mode === "auto" ? AUTO_RATES : [Number(msg.mode)];
    var input = new Uint8Array(msg.buffer);
    var tried = [], best = null, noResampleSeen = false;

    for (var i = 0; i < rates.length; i++) {
      var rate = rates[i];
      // если на предыдущем проходе ничего не передискретизировалось, то более низкая частота
      // выше исходной ничего не изменит — такие проходы пропускаем
      if (best && noResampleSeen && rate >= best.maxSrcRate) continue;

      var opts = Object.assign({}, msg.opts, { rate: rate });
      var pass = i + 1, passes = rates.length;
      var res = S.shrink(input, opts, function (p) {
        self.postMessage({ type: "progress", pass: pass, passes: passes, rate: rate, done: p.done, total: p.total, name: p.name });
      });
      var maxSrc = 0;
      res.stats.samples.forEach(function (s) { if (s.srcRate > maxSrc) maxSrc = s.srcRate; });
      noResampleSeen = noResampleSeen || rate >= maxSrc;
      tried.push({ rate: rate, bytes: res.data.length });
      best = { data: res.data, stats: res.stats, maxSrcRate: maxSrc };
      if (res.data.length <= S.DEVICE_LIMIT) break;      // влезло — дальше снижать качество незачем
    }

    var out = best.data;
    var buf = out.buffer.byteLength === out.length ? out.buffer : out.buffer.slice(out.byteOffset, out.byteOffset + out.length);
    self.postMessage({ type: "done", buffer: buf, stats: best.stats, tried: tried }, [buf]);
  } catch (err) {
    self.postMessage({
      type: "error",
      code: err && err.code ? err.code : "internal",
      args: err && err.args ? err.args : {},
      message: String(err && err.message ? err.message : err)
    });
  }
};
