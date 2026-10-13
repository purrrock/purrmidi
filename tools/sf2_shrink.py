#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sf2_shrink.py — уменьшение SoundFont2 (.sf2): стерео -> моно, передискретизация до 24 кГц.

Что делает
----------
* Стереопары (сэмплы типа Left/Right, связанные через sampleLink) сводятся в один моно-сэмпл.
  Простое среднее (L+R)/2 для таких банков опасно: каналы часто записаны разнесённой парой
  микрофонов (задержка до ~1.5 мс, местами обратная полярность), и сумма «как есть» гасит часть
  нот на 5-14 дБ. Поэтому правый канал перед суммированием выравнивается по времени и полярности
  к левому (кросс-корреляция в пределах ±--maxlag отсчётов, отдельно для «тела» сэмпла и для
  зацикленной части; внутри цикла сдвиг циклический, поэтому стык цикла остаётся бесшовным).
  Затем громкость нормируется так, чтобы суммарная мощность моно-сэмпла (с учётом закона
  панорамы TinySoundFont: -3 дБ на канал в центре) совпадала с мощностью исходной пары.
  Таким образом профиль громкости по клавиатуре сохраняется. --gain-db добавляет сверху.
* Зоны инструмента (ibag/igen/imod): зона правого канала, дублирующая зону левого
  (те же key/vel range, root key, огибающие и т.д., отличается только pan и sampleID), удаляется;
  в оставшейся зоне убирается pan (центр) и sampleID переписывается на новый моно-сэмпл.
  Пресеты (phdr/pbag/pgen), инструменты (inst), диапазоны нот и скорости, корневые ноты,
  огибающие, режимы цикла (sampleModes) и остальные генераторы остаются без изменений.
* Передискретизация до --rate (по умолчанию 24000 Гц) оконным sinc-фильтром (Кайзер) с ФНЧ
  чуть ниже новой частоты Найквиста. Сэмплы с частотой <= --rate не передискретизируются.
* Циклы сохраняются ТОЧНО: длина цикла в новой частоте — целое число отсчётов, а сетка
  передискретизации подбирается так, чтобы начало цикла попадало ровно в целый отсчёт.
  Поэтому стык цикла остаётся непрерывным (никаких щелчков от округления позиций цикла).
  Платой за это является микроскопическое изменение шага сэмпла (обычно доли цента); оно
  компенсируется полем pitchCorrection в заголовке сэмпла (TinySoundFont и FluidSynth его учитывают).
* Остаётся 16 бит, после каждого сэмпла — 46 нулевых отсчётов (требование спецификации SF2).

Зависимости: Python 3.8+, numpy.

Использование:
    python3 sf2_shrink.py SNDFNT.SF2 SNDFNT_small.SF2
    python3 sf2_shrink.py in.sf2 out.sf2 --rate 24000 --gain-db 3.01 -v
"""

import argparse
import math
import struct
import sys

import numpy as np

# ----------------------------------------------------------------------------------------------
#  Константы SF2
# ----------------------------------------------------------------------------------------------
GEN_PAN = 17
GEN_SAMPLE_ID = 53
# Генераторы смещений адресов сэмпла (в отсчётах исходного сэмпла) — после передискретизации
# они потеряли бы смысл, поэтому при их наличии программа останавливается.
GEN_ADDR_OFFSETS = {0, 1, 2, 3, 4, 12, 45, 50}

SAMPLE_MONO, SAMPLE_RIGHT, SAMPLE_LEFT = 1, 2, 4
SAMPLE_LINKED, SAMPLE_ROM_FLAG = 8, 0x8000

SHDR_FMT = "<20sIIIIIBbHH"
SHDR_SIZE = 46
PAD_SAMPLES = 46


class SF2Error(Exception):
    pass


# ----------------------------------------------------------------------------------------------
#  RIFF / SF2 разбор и сборка
# ----------------------------------------------------------------------------------------------
def read_chunks(buf, pos, end):
    """Список (id, смещение данных, размер) для чанков в диапазоне [pos, end)."""
    out = []
    while pos + 8 <= end:
        cid = bytes(buf[pos:pos + 4])
        size = struct.unpack_from("<I", buf, pos + 4)[0]
        if pos + 8 + size > end:
            raise SF2Error("Повреждённый чанк %r (выходит за границы файла)" % cid)
        out.append((cid, pos + 8, size))
        pos += 8 + size + (size & 1)
    return out


def make_chunk(cid, payload):
    pad = b"\0" if len(payload) & 1 else b""
    return cid + struct.pack("<I", len(payload)) + payload + pad


class SoundFont:
    """Только нужные нам части файла."""

    def __init__(self, data):
        if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"sfbk":
            raise SF2Error("Это не SF2-файл (нет RIFF/sfbk)")
        riff_size = struct.unpack_from("<I", data, 4)[0]
        end = min(len(data), 8 + riff_size)

        self.info_payload = None
        self.smpl = None
        self.has_sm24 = False
        pdta = {}
        for cid, off, size in read_chunks(data, 12, end):
            if cid != b"LIST":
                continue
            kind = bytes(data[off:off + 4])
            subs = read_chunks(data, off + 4, off + size)
            if kind == b"INFO":
                self.info_payload = bytes(data[off:off + size])
            elif kind == b"sdta":
                for scid, soff, ssize in subs:
                    if scid == b"smpl":
                        self.smpl = np.frombuffer(data, dtype="<i2", count=ssize // 2, offset=soff)
                    elif scid == b"sm24":
                        self.has_sm24 = True
            elif kind == b"pdta":
                for scid, soff, ssize in subs:
                    pdta[scid] = bytes(data[soff:soff + ssize])

        for need in (b"phdr", b"pbag", b"pmod", b"pgen", b"inst", b"ibag", b"imod", b"igen", b"shdr"):
            if need not in pdta:
                raise SF2Error("Нет чанка %s в pdta" % need.decode())
        if self.smpl is None:
            raise SF2Error("Нет чанка smpl (в файле нет PCM-данных)")
        if self.info_payload is None:
            self.info_payload = b"INFO"

        self.phdr, self.pbag, self.pmod, self.pgen = (pdta[k] for k in (b"phdr", b"pbag", b"pmod", b"pgen"))
        self.inst_raw = pdta[b"inst"]

        # shdr
        raw = pdta[b"shdr"]
        n = len(raw) // SHDR_SIZE
        self.shdr = [struct.unpack_from(SHDR_FMT, raw, i * SHDR_SIZE) for i in range(n)]
        self.shdr_terminal = raw[(n - 1) * SHDR_SIZE:n * SHDR_SIZE]
        self.shdr = self.shdr[:-1]  # без EOS

        # инструменты: (name20, bagNdx)
        n_inst = len(self.inst_raw) // 22
        self.inst = [struct.unpack_from("<20sH", self.inst_raw, i * 22) for i in range(n_inst)]

        # ibag / igen / imod
        raw = pdta[b"ibag"]
        self.ibag = [struct.unpack_from("<HH", raw, i * 4) for i in range(len(raw) // 4)]
        raw = pdta[b"igen"]
        self.igen = [(struct.unpack_from("<H", raw, i * 4)[0], raw[i * 4 + 2:i * 4 + 4])
                     for i in range(len(raw) // 4)]
        self.igen_terminal = raw[-4:]
        raw = pdta[b"imod"]
        self.imod = [raw[i * 10:(i + 1) * 10] for i in range(len(raw) // 10)]
        self.imod_terminal = raw[-10:]

    def sample_name(self, i):
        return self.shdr[i][0].split(b"\0")[0].decode("latin-1")


# ----------------------------------------------------------------------------------------------
#  Передискретизация: оконный sinc с таблицей дробных фаз
# ----------------------------------------------------------------------------------------------
class Interpolator:
    """Band-limited интерполятор для произвольных (дробных) позиций в исходном сигнале."""

    NPHASE = 512
    CHUNK = 8192

    def __init__(self, half_width=96, beta=9.0):
        self.T = int(half_width)
        self.beta = float(beta)
        self._tables = {}

    def _table(self, cutoff):
        """cutoff — частота среза в циклах на ИСХОДНЫЙ отсчёт (0..0.5)."""
        key = round(cutoff, 9)
        tab = self._tables.get(key)
        if tab is not None:
            return tab
        T, NP = self.T, self.NPHASE
        j = np.arange(-T + 1, T + 1, dtype=np.float64)                 # отсчёты ядра
        frac = (np.arange(NP + 1, dtype=np.float64) / NP)[:, None]     # дробная фаза
        tau = j[None, :] - frac                                        # расстояние до точки интерполяции
        w = np.i0(self.beta * np.sqrt(np.clip(1.0 - (tau / T) ** 2, 0.0, 1.0))) / np.i0(self.beta)
        tab = 2.0 * cutoff * np.sinc(2.0 * cutoff * tau) * w
        tab /= tab.sum(axis=1, keepdims=True)                          # единичное усиление на DC
        self._tables[key] = tab
        return tab

    def evaluate(self, x, pos, cutoff):
        """Значения сигнала x в дробных позициях pos (в отсчётах x)."""
        T, NP = self.T, self.NPHASE
        tab = self._table(cutoff)
        n_needed = int(np.floor(pos.max())) + 2 * T + 2 if len(pos) else 0
        xpad = np.zeros(max(n_needed, len(x) + 2 * T + 2), dtype=np.float64)
        xpad[T:T + len(x)] = x                                          # xpad[T + i] = x[i]
        taps = np.arange(2 * T)[None, :]
        out = np.empty(len(pos), dtype=np.float64)
        for s in range(0, len(pos), self.CHUNK):
            p = pos[s:s + self.CHUNK]
            i = np.floor(p).astype(np.int64)
            q = (p - i) * NP
            q0 = np.minimum(q.astype(np.int64), NP - 1)
            a = (q - q0)[:, None]
            coef = tab[q0] * (1.0 - a) + tab[q0 + 1] * a
            idx = (i + 1)[:, None] + taps                               # i + T + j,  j = -T+1 .. T
            out[s:s + len(p)] = np.einsum("ij,ij->i", xpad[idx], coef)
        return out


class Resampled:
    __slots__ = ("data", "loop_start", "loop_end", "cents_err", "ratio")


def resample_sample(interp, x, src_rate, tgt_rate, loop, lowpass):
    """
    x        — float64, исходный моно-сигнал (отсчёты от начала сэмпла)
    loop     — (ls, le) относительно начала сэмпла или None; длина цикла le-ls
    Возвращает Resampled: data(float64), loop_start/loop_end (относительно начала), cents_err.
    """
    n = len(x)
    res = Resampled()
    nominal = src_rate / float(tgt_rate)             # исходных отсчётов на один новый
    res.ratio = 1.0 / nominal
    cutoff = 0.5 / nominal * lowpass                 # циклов на исходный отсчёт

    if loop is not None:
        ls, le = loop
        L = le - ls
        Lp = max(1, int(round(L / nominal)))         # длина цикла в новой частоте — ЦЕЛАЯ
        rho = L / float(Lp)                          # исходных отсчётов на новый (≈ nominal)
        sp = int(math.floor(ls / rho))               # новый индекс начала цикла
        phi = sp - ls / rho                          # (-1, 0]: сдвиг сетки, p(sp) == ls точно
        n_nat = int(math.ceil(n / rho + phi))
        n_out = max(n_nat, sp + Lp + 8)
    else:
        rho, phi, n_nat = nominal, 0.0, int(math.ceil(n / nominal))
        n_out = n_nat

    pos = (np.arange(n_out, dtype=np.float64) - phi) * rho
    y = interp.evaluate(x, pos, cutoff)

    if loop is not None:
        # если исходник кончился раньше, чем нужно 8 точек после конца цикла — достраиваем периодически
        for k in range(max(sp + Lp, n_nat), n_out):
            y[k] = y[k - Lp]
        res.loop_start, res.loop_end = sp, sp + Lp
    else:
        res.loop_start = res.loop_end = None

    res.data = y
    res.cents_err = 1200.0 * math.log2(rho / nominal)   # >0: после воспроизведения тон выше исходного
    return res


# ----------------------------------------------------------------------------------------------
#  Сведение стереопары в моно с выравниванием каналов
# ----------------------------------------------------------------------------------------------
def best_lag(a, b, circular, maxlag, min_corr):
    """
    Лаг k (|k| <= maxlag) и знак, при которых a[i] и sign*b[i+k] максимально коррелируют.
    Среди почти равных максимумов выбирается лаг с наименьшим |k| (минимальное смещение во времени).
    Сдвиг применяется только если он повышает |корреляцию| хотя бы на 0.1 относительно нулевого.
    Возвращает (lag, sign, corr). Если корреляция слабая (< min_corr) — (0, +1, corr0).
    """
    n = len(a)
    if n < 4 * maxlag + 16:
        return 0, 1, 0.0
    norm = math.sqrt(float(np.dot(a, a)) * float(np.dot(b, b))) + 1e-9
    size = n if circular else 1 << int(math.ceil(math.log2(2 * n)))
    c = np.fft.irfft(np.conj(np.fft.rfft(a, size)) * np.fft.rfft(b, size), size) / norm   # c[k]=sum a[i]*b[i+k]
    lags = np.arange(-maxlag, maxlag + 1)
    cv = c[lags % size]
    m = float(np.max(np.abs(cv)))
    c0 = float(cv[maxlag])
    if m < min_corr:
        return 0, 1, c0
    if m - abs(c0) < 0.1:                      # сдвиг почти ничего не даёт — не двигаем, только полярность
        return 0, (1 if c0 >= 0 else -1), c0
    cand = lags[np.abs(cv) >= m - 0.03]
    lag = int(cand[np.argmin(np.abs(cand))])
    val = float(cv[lag + maxlag])
    return lag, (1 if val >= 0 else -1), val


def shifted(b, lag, loop):
    """b[n+lag]; внутри цикла [ls, le) сдвиг циклический (сохраняет периодичность цикла)."""
    n = len(b)
    pos = np.arange(n)
    idx = pos + lag
    if loop is not None:
        ls, le = loop
        m = (pos >= ls) & (pos < le)
        idx[m] = ls + ((pos[m] - ls + lag) % (le - ls))
    out = np.zeros(n, dtype=np.float64)
    ok = (idx >= 0) & (idx < n)
    out[ok] = b[idx[ok]]
    return out


def mix_pair(left, right, loop, maxlag, min_corr, align):
    """Возвращает (left + выровненный right, описание)."""
    n = len(left)
    if not align or maxlag <= 0:
        return left + right, "без выравнивания"
    if loop is not None and (loop[1] - loop[0]) >= 4 * maxlag + 16 and loop[0] > 0:
        ls, le = loop
        lag_l, sg_l, c_l = best_lag(left[ls:le], right[ls:le], True, maxlag, min_corr)
        lag_b, sg_b, c_b = best_lag(left[:ls], right[:ls], False, maxlag, min_corr)
        rb = sg_b * shifted(right, lag_b, None)
        rl = sg_l * shifted(right, lag_l, (ls, le))
        w = np.ones(n)
        w[:ls] = 0.0
        x = min(2048, ls)
        w[ls - x:ls] = np.linspace(0.0, 1.0, x, endpoint=False)    # плавный переход тело -> цикл
        info = "тело: лаг %+d (%s%.2f), цикл: лаг %+d (%s%.2f)" % (
            lag_b, "-" if sg_b < 0 else "+", abs(c_b), lag_l, "-" if sg_l < 0 else "+", abs(c_l))
        return left + (1.0 - w) * rb + w * rl, info
    lag, sg, c = best_lag(left, right, False, maxlag, min_corr)
    return left + sg * shifted(right, lag, None), "лаг %+d (%s%.2f)" % (lag, "-" if sg < 0 else "+", abs(c))


# ----------------------------------------------------------------------------------------------
#  Основная логика
# ----------------------------------------------------------------------------------------------
def find_groups(sf):
    """
    Группы сэмплов: либо стереопара (L, R), либо одиночный сэмпл.
    Возвращает список групп [(old_ids...)] в порядке первого появления и map old_id -> group index.
    """
    n = len(sf.shdr)
    types = [h[9] for h in sf.shdr]
    links = [h[8] for h in sf.shdr]
    for i, t in enumerate(types):
        if t & SAMPLE_ROM_FLAG:
            raise SF2Error("Сэмпл #%d (%s) — ROM-сэмпл, не поддерживается" % (i, sf.sample_name(i)))

    partner = {}
    for i in range(n):
        if types[i] == SAMPLE_LEFT:
            j = links[i]
            if j < n and j != i and types[j] == SAMPLE_RIGHT and links[j] == i:
                partner[i] = j
                partner[j] = i

    groups, gmap = [], {}
    for i in range(n):
        if i in gmap:
            continue
        members = (i, partner[i]) if i in partner else (i,)
        # левый канал всегда первым
        if len(members) == 2 and types[members[0]] != SAMPLE_LEFT:
            members = (members[1], members[0])
        for m in members:
            gmap[m] = len(groups)
        groups.append(members)
    return groups, gmap


def mono_name(sf, members):
    name = sf.sample_name(members[0])
    if len(members) == 2:
        r = sf.sample_name(members[1])
        if name[-1:] in ("L", "l") and r[:-1] == name[:-1]:
            name = name[:-1].rstrip(" _-.")
    return name[:19].encode("latin-1", "replace")


def shrink(sf, tgt_rate, gain_db, half_width, lowpass, maxlag, min_corr, align, verbose):
    smpl = sf.smpl
    n_total = len(smpl)
    groups, gmap = find_groups(sf)
    gain_lin = 10.0 ** (gain_db / 20.0)
    interp = Interpolator(half_width=half_width)

    # --- проверки зон ---
    for z, (g_idx, _) in enumerate(sf.ibag[:-1]):
        end = sf.ibag[z + 1][0]
        gens = sf.igen[g_idx:end]
        if any(op == GEN_SAMPLE_ID for op, _ in gens):
            for op, raw in gens:
                if op in GEN_ADDR_OFFSETS and struct.unpack("<h", raw)[0] != 0:
                    raise SF2Error("Зона #%d использует генератор смещения адреса сэмпла (%d): "
                                   "не поддерживается" % (z, op))

    # --- передискретизация каждой группы ---
    results = []          # на группу: dict(res, name, orig_pitch, pitch_corr, merged, ...)
    for gi, members in enumerate(groups):
        L = sf.shdr[members[0]]
        name, st, en, ls_abs, le_abs, rate, pitch, corr = L[0], L[1], L[2], L[3], L[4], L[5], L[6], L[7]
        if not (0 <= st < en <= n_total):
            raise SF2Error("Сэмпл #%d (%s): некорректные start/end" % (members[0], sf.sample_name(members[0])))
        left = smpl[st:en].astype(np.float64)
        warn = []
        mixdesc = ""
        if len(members) == 2:
            R = sf.shdr[members[1]]
            right = smpl[R[1]:R[2]].astype(np.float64)
            if len(right) != len(left):
                m = min(len(left), len(right))
                warn.append("длины L/R различаются (%d/%d) — обрезано до %d" % (len(left), len(right), m))
                left, right = left[:m], right[:m]
            if (R[3] - R[1], R[4] - R[1]) != (ls_abs - st, le_abs - st):
                warn.append("петли L/R различаются — взяты петли левого канала")
            if R[5] != rate:
                warn.append("частоты L/R различаются — взята частота левого")
            mixed, mixinfo = mix_pair(left, right, (ls_abs - st, le_abs - st) if 0 <= ls_abs - st < le_abs - st <= len(left) else None,
                                      maxlag, min_corr, align)
            rms_l = math.sqrt(float(np.mean(left ** 2)))
            rms_r = math.sqrt(float(np.mean(right ** 2)))
            rms_m = math.sqrt(float(np.mean(mixed ** 2))) + 1e-9
            # суммарная мощность моно (по 0.707 на канал) = мощность пары: rms_m' = sqrt(rms_l^2 + rms_r^2)
            factor = math.sqrt(rms_l ** 2 + rms_r ** 2) / rms_m
            if not (0.25 <= factor <= 2.0):
                warn.append("нормировка громкости %.2f вне [0.25; 2.0] — ограничена" % factor)
                factor = min(2.0, max(0.25, factor))
            x = mixed * factor * gain_lin
            mixdesc = "%s; множитель %.3f" % (mixinfo, factor)
        else:
            x = left
        n = len(x)

        ls, le = ls_abs - st, le_abs - st
        loop = (ls, le) if (0 <= ls < le <= n) else None

        if rate > tgt_rate:
            res = resample_sample(interp, x, rate, tgt_rate, loop, lowpass)
            out_rate = tgt_rate
        else:
            # уже достаточно низкая частота — оставляем как есть
            res = Resampled()
            res.data, res.ratio, res.cents_err = x, 1.0, 0.0
            res.loop_start, res.loop_end = (loop if loop else (None, None))
            out_rate = rate

        # если циклов нет — масштабируем «как есть» поля, чтобы не терять значения
        if loop is None:
            res.loop_start = int(round(max(0, ls) * res.ratio))
            res.loop_end = int(round(max(0, le) * res.ratio))
            res.loop_start = min(res.loop_start, len(res.data))
            res.loop_end = min(res.loop_end, len(res.data))

        # компенсация микро-расхождения шага через pitchCorrection
        new_corr = corr + int(round(-res.cents_err))
        new_corr = max(-99, min(99, new_corr))
        resid = res.cents_err + (new_corr - corr)

        results.append(dict(members=members, name=mono_name(sf, members), res=res, rate=out_rate,
                            pitch=pitch, corr=new_corr, resid=resid, warn=warn,
                            merged=len(members) == 2, n_src=n, loop=loop, mixdesc=mixdesc))

    # --- защита от клиппинга (равномерная для всех склеенных стереопар) ---
    peak = 0.0
    for r in results:
        if r["merged"]:
            peak = max(peak, float(np.max(np.abs(r["res"].data))) if len(r["res"].data) else 0.0)
    guard = 1.0
    if peak > 32767.0 * 0.999:
        guard = 32767.0 * 0.999 / peak
        for r in results:
            if r["merged"]:
                r["res"].data = r["res"].data * guard
    # --- квантование и раскладка в новый smpl ---
    chunks, pos = [], 0
    new_shdr = []
    for r in results:
        d = np.clip(np.rint(r["res"].data), -32768, 32767).astype("<i2")
        start, end = pos, pos + len(d)
        res = r["res"]
        ls = start + int(res.loop_start)
        le = start + int(res.loop_end)
        new_shdr.append(struct.pack(SHDR_FMT, r["name"].ljust(20, b"\0"), start, end, ls, le,
                                    r["rate"], r["pitch"], r["corr"], 0, SAMPLE_MONO))
        chunks.append(d)
        chunks.append(np.zeros(PAD_SAMPLES, dtype="<i2"))
        pos = end + PAD_SAMPLES
        r["start"], r["end"], r["ls"], r["le"] = start, end, ls, le
    new_smpl = np.concatenate(chunks).astype("<i2")
    new_shdr_bytes = b"".join(new_shdr) + sf.shdr_terminal

    # --- пересборка ibag / igen / imod ---
    new_ibag, new_igen, new_imod = [], [], []
    new_inst = []
    n_removed = 0
    n_inst = len(sf.inst) - 1                     # последняя запись inst — терминальная (EOI)
    for ii in range(n_inst):
        name, bag0 = sf.inst[ii]
        bag1 = sf.inst[ii + 1][1]
        new_inst.append(struct.pack("<20sH", name, len(new_ibag)))
        seen = set()
        for z in range(bag0, bag1):
            g0, m0 = sf.ibag[z]
            g1, m1 = sf.ibag[z + 1]
            gens = sf.igen[g0:g1]
            mods = sf.imod[m0:m1]
            sid = None
            for op, raw in gens:
                if op == GEN_SAMPLE_ID:
                    sid = struct.unpack("<H", raw)[0]
            if sid is None:                       # глобальная зона инструмента
                new_ibag.append((len(new_igen), len(new_imod)))
                new_igen.extend(gens)
                new_imod.extend(mods)
                continue
            if sid >= len(sf.shdr):
                raise SF2Error("Зона #%d ссылается на несуществующий сэмпл %d" % (z, sid))
            gi = gmap[sid]
            if results[gi]["merged"]:
                core = tuple((op, raw) for op, raw in gens if op not in (GEN_PAN, GEN_SAMPLE_ID))
                sig = (gi, core, tuple(mods))
                if sig in seen:                   # дубль зоны правого канала
                    n_removed += 1
                    continue
                seen.add(sig)
                gens = [(op, raw) for op, raw in gens if op != GEN_PAN]
            gens = [(op, struct.pack("<H", gi) if op == GEN_SAMPLE_ID else raw) for op, raw in gens]
            new_ibag.append((len(new_igen), len(new_imod)))
            new_igen.extend(gens)
            new_imod.extend(mods)
    # терминальные записи
    new_inst.append(struct.pack("<20sH", sf.inst[-1][0], len(new_ibag)))
    new_ibag.append((len(new_igen), len(new_imod)))

    ibag_b = b"".join(struct.pack("<HH", a, b) for a, b in new_ibag)
    igen_b = b"".join(struct.pack("<H", op) + raw for op, raw in new_igen) + sf.igen_terminal
    imod_b = b"".join(new_imod) + sf.imod_terminal
    inst_b = b"".join(new_inst)

    # --- сборка файла ---
    pdta = b"pdta" + b"".join([
        make_chunk(b"phdr", sf.phdr), make_chunk(b"pbag", sf.pbag),
        make_chunk(b"pmod", sf.pmod), make_chunk(b"pgen", sf.pgen),
        make_chunk(b"inst", inst_b), make_chunk(b"ibag", ibag_b),
        make_chunk(b"imod", imod_b), make_chunk(b"igen", igen_b),
        make_chunk(b"shdr", new_shdr_bytes)])
    sdta = b"sdta" + make_chunk(b"smpl", new_smpl.tobytes())
    body = b"sfbk" + make_chunk(b"LIST", sf.info_payload) + make_chunk(b"LIST", sdta) + make_chunk(b"LIST", pdta)
    out = b"RIFF" + struct.pack("<I", len(body)) + body

    stats = dict(groups=len(groups), merged=sum(1 for r in results if r["merged"]),
                 zones_removed=n_removed, guard=guard, peak=peak, results=results,
                 old_samples=n_total, new_samples=len(new_smpl))
    return out, stats


def validate(path):
    """Пересчитывает структуру готового файла и проверяет базовые инварианты SF2."""
    with open(path, "rb") as f:
        data = f.read()
    sf = SoundFont(data)
    n = len(sf.smpl)
    if struct.unpack_from("<I", data, 4)[0] != len(data) - 8:
        raise SF2Error("Размер RIFF не совпадает с размером файла")
    for i, h in enumerate(sf.shdr):
        _, st, en, ls, le, rate, _, _, link, typ = h
        if not (0 <= st < en <= n):
            raise SF2Error("shdr[%d]: start/end вне smpl" % i)
        if en + PAD_SAMPLES > n or np.any(sf.smpl[en:en + PAD_SAMPLES] != 0):
            raise SF2Error("shdr[%d]: нет 46 нулевых отсчётов после сэмпла" % i)
        if not (st <= ls <= le <= en):
            raise SF2Error("shdr[%d]: цикл вне сэмпла" % i)
        if typ != SAMPLE_MONO or link != 0:
            raise SF2Error("shdr[%d]: ожидался моно-сэмпл" % i)
    for z in range(len(sf.ibag) - 1):
        if sf.ibag[z][0] > sf.ibag[z + 1][0] or sf.ibag[z][1] > sf.ibag[z + 1][1]:
            raise SF2Error("ibag: индексы не монотонны")
        for op, raw in sf.igen[sf.ibag[z][0]:sf.ibag[z + 1][0]]:
            if op == GEN_SAMPLE_ID and struct.unpack("<H", raw)[0] >= len(sf.shdr):
                raise SF2Error("igen: sampleID вне диапазона")
    if sf.ibag[-1][0] != len(sf.igen) - 1 or sf.ibag[-1][1] != len(sf.imod) - 1:
        raise SF2Error("ibag: терминальная запись указывает не на терминальные igen/imod")
    return len(sf.shdr), len(sf.ibag) - 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Уменьшение SF2: моно + передискретизация (с сохранением зон и циклов)")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--rate", type=int, default=24000, help="целевая частота дискретизации, Гц (24000)")
    ap.add_argument("--gain-db", type=float, default=0.0,
                    help="дополнительное усиление склеенных моно-сэмплов, дБ (0 = мощность пары сохраняется)")
    ap.add_argument("--maxlag", type=int, default=64,
                    help="макс. сдвиг R относительно L при выравнивании, отсчётов исходной частоты (64)")
    ap.add_argument("--min-corr", type=float, default=0.4,
                    help="минимальная корреляция L/R, при которой выполняется выравнивание (0.4)")
    ap.add_argument("--no-align", action="store_true", help="не выравнивать каналы по времени/полярности")
    ap.add_argument("--taps", type=int, default=96, help="полуширина sinc-фильтра в исходных отсчётах (96)")
    ap.add_argument("--lowpass", type=float, default=0.96,
                    help="центр полосы перехода ФНЧ как доля новой частоты Найквиста (0.96)")
    ap.add_argument("-v", "--verbose", action="store_true", help="подробный отчёт по каждому сэмплу")
    args = ap.parse_args(argv)

    with open(args.input, "rb") as f:
        data = f.read()
    sf = SoundFont(data)
    if sf.has_sm24:
        print("ВНИМАНИЕ: чанк sm24 (24-битные расширения) отбрасывается, выход — 16 бит.", file=sys.stderr)

    out, st = shrink(sf, args.rate, args.gain_db, args.taps, args.lowpass, args.maxlag, args.min_corr,
                      not args.no_align, args.verbose)
    with open(args.output, "wb") as f:
        f.write(out)
    n_s, n_z = validate(args.output)

    print("Вход : %s  %.2f МиБ  (%d сэмплов, %d отсч. PCM)" % (args.input, len(data) / 1048576, len(sf.shdr), st["old_samples"]))
    print("Выход: %s  %.2f МиБ  (%d сэмплов, %d отсч. PCM)  -> в %.2f раза меньше"
          % (args.output, len(out) / 1048576, n_s, st["new_samples"], len(data) / len(out)))
    print("Стереопар склеено в моно: %d; удалено дублирующих зон: %d; зон в инструментах: %d"
          % (st["merged"], st["zones_removed"], n_z))
    print("Пик после сведения %.0f%s"
          % (st["peak"],
             "" if st["guard"] == 1.0 else "  -> общее ослабление %.3f (защита от клиппинга)" % st["guard"]))
    worst = max((abs(r["resid"]) for r in st["results"]), default=0.0)
    print("Максимальная остаточная расстройка тона из-за точной подгонки циклов: %.3f цента" % worst)
    for r in st["results"]:
        for w in r["warn"]:
            print("ВНИМАНИЕ [%s]: %s" % (r["name"].decode("latin-1"), w), file=sys.stderr)
    if args.verbose:
        print("\n%-20s %8s %8s %16s %16s %7s  %s" % ("сэмпл", "было", "стало", "цикл было", "цикл стало", "цент", "сведение"))
        for r in st["results"]:
            lp = r["loop"]
            old = "%d..%d" % lp if lp else "-"
            new = "%d..%d" % (r["ls"] - r["start"], r["le"] - r["start"])
            print("%-20s %8d %8d %16s %16s %+7.3f  %s" % (r["name"].decode("latin-1"), r["n_src"],
                                                         r["end"] - r["start"], old, new, r["resid"], r["mixdesc"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SF2Error as e:
        print("Ошибка: %s" % e, file=sys.stderr)
        sys.exit(1)
