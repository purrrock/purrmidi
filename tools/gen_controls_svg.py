#!/usr/bin/env python3
"""Генерирует иллюстрацию «ручки и колёса клавиатуры -> что они делают» (RU и EN).

Запуск из корня репозитория:  python3 tools/gen_controls_svg.py
Результат: docs/controls_{ru,en}.svg (для README и docs) и site/controls_{ru,en}.svg (для сайта).
Тексты — в словаре T ниже; при изменении таблицы CC в docs/ENGINES.md обновите их и пересоберите.
"""

T = {
 "ru": {
  "title": "Что делают ручки и колёса вашей MIDI-клавиатуры",
  "desc": "Схема типовой MIDI-клавиатуры: колесо высоты тона, мод-колесо, ручки громкости и яркости, кнопка смены инструмента и педаль сустейна, с пояснением, что каждый элемент делает в PurrMidi.",
  "head": "Что делают ручки и колёса клавиатуры в PurrMidi",
  "sub": "Названия на вашей клавиатуре могут отличаться; важен номер CC. Что из этого есть именно у вас, зависит от модели.",
  "pitch": "Колесо высоты тона", "pitch_d": "Pitch Bend: ±2 полутона, везде",
  "mod": "Мод-колесо", "mod_cc": "CC1", "mod_d": "«Живее, ярче»; в нуле звук не меняется",
  "vol": "Громкость", "vol_cc": "CC7", "vol_d": "Общая громкость, везде",
  "bri": "Яркость / срез", "bri_cc": "CC74", "bri_d": "Яркость тембра; у Lead — срез фильтра",
  "res": "Резонанс", "res_cc": "CC71", "res_d": "Яркость или резонанс (по инструменту)",
  "rel": "Отпускание", "rel_cc": "CC72", "rel_d": "Как долго звук затухает после клавиши",
  "pc": "Кнопка смены звука", "pc_cc": "Program Change", "pc_d": "Выбор инструмента (127 = следующий)",
  "sus": "Педаль сустейна", "sus_cc": "CC64", "sus_d": "Звук тянется, пока педаль нажата (у органа нет)",
  "note": "Не нашли свою ручку? Она может отправлять другой CC: полный список в docs/ENGINES.md",
  "keys": "клавиши",
 },
 "en": {
  "title": "What the knobs and wheels of your MIDI keyboard do",
  "desc": "Diagram of a typical MIDI keyboard: pitch wheel, mod wheel, volume and brightness knobs, an instrument-change button and a sustain pedal, with a note on what each one does in PurrMidi.",
  "head": "What your keyboard's knobs and wheels do in PurrMidi",
  "sub": "Labels on your keyboard may differ; the CC number is what matters. Which of these you have depends on the model.",
  "pitch": "Pitch wheel", "pitch_d": "Pitch Bend: ±2 semitones, everywhere",
  "mod": "Mod wheel", "mod_cc": "CC1", "mod_d": "“Livelier, brighter”; at zero the sound is unchanged",
  "vol": "Volume", "vol_cc": "CC7", "vol_d": "Master volume, everywhere",
  "bri": "Brightness / cutoff", "bri_cc": "CC74", "bri_d": "Tone brightness; filter cutoff on Lead",
  "res": "Resonance", "res_cc": "CC71", "res_d": "Brightness or resonance (per instrument)",
  "rel": "Release", "rel_cc": "CC72", "rel_d": "How long the sound fades after the key",
  "pc": "Instrument button", "pc_cc": "Program Change", "pc_d": "Pick an instrument (127 = next)",
  "sus": "Sustain pedal", "sus_cc": "CC64", "sus_d": "Sound rings while pressed (no sustain on Organ)",
  "note": "Can't find your knob? It may send another CC: full list in docs/ENGINES.md (Russian)",
  "keys": "keys",
 },
}

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def wrap(text, width):
    lines, cur = [], ""
    for wd in text.split():
        if cur and len(cur) + len(wd) + 1 > width:
            lines.append(cur); cur = wd
        else:
            cur = (cur + " " + wd).strip()
    lines.append(cur)
    return lines

def build(lang):
    t = T[lang]
    W, H = 900, 590
    o = []
    a = o.append
    a(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" aria-labelledby="t d">')
    a(f'<title id="t">{esc(t["title"])}</title>')
    a(f'<desc id="d">{esc(t["desc"])}</desc>')
    a('<style>'
      'text{font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;fill:#2a2a38}'
      '.h{font-size:22px;font-weight:700}.s{font-size:13px;fill:#5d5d70}'
      '.l{font-size:15px;font-weight:700}.d{font-size:13px;fill:#5d5d70}'
      '.cc{font-size:12.5px;font-weight:700;fill:#fff;text-anchor:middle}'
      '.num{font-size:13px;font-weight:700;fill:#fff;text-anchor:middle}'
      '.n{font-size:12.5px;fill:#5d5d70}'
      '</style>')
    a(f'<rect x="0" y="0" width="{W}" height="{H}" rx="20" fill="#fffaf5" stroke="#efe6dd"/>')
    a(f'<text class="h" x="30" y="42">{esc(t["head"])}</text>')
    a(f'<text class="s" x="30" y="64">{esc(t["sub"])}</text>')

    # --- клавиатура ---
    kx, ky, kw, kh = 130, 106, 640, 140
    a(f'<rect x="{kx}" y="{ky}" width="{kw}" height="{kh}" rx="14" fill="#3b3b4f"/>')
    a(f'<rect x="{kx+10}" y="{ky+10}" width="{kw-20}" height="58" rx="8" fill="#4b4b63"/>')
    key_top = ky + 78
    n_white = 22
    kwid = (kw - 20) / n_white
    for i in range(n_white):
        x = kx + 10 + i * kwid
        a(f'<rect x="{x:.1f}" y="{key_top}" width="{kwid-1.5:.1f}" height="{kh-88}" rx="2" fill="#fffdf9"/>')
    for i in range(n_white - 1):
        if i % 7 in (2, 6):
            continue
        x = kx + 10 + (i + 1) * kwid - kwid * 0.3
        a(f'<rect x="{x:.1f}" y="{key_top}" width="{kwid*0.6:.1f}" height="{(kh-88)*0.6:.1f}" rx="2" fill="#2a2a38"/>')

    pink, blue, green, orange, purple = "#e0668f", "#5a74d6", "#2f9b80", "#c97a12", "#8a6cd6"
    cy = ky + 39  # центр панели
    pw_x, mw_x = kx + 38, kx + 76
    knob_x = [kx + 190, kx + 280, kx + 370, kx + 460]
    btn_x = kx + 560
    ped_x, ped_y = kx + kw // 2, ky + kh + 14

    # колёса
    a(f'<rect x="{pw_x-11}" y="{ky+14}" width="22" height="50" rx="8" fill="{pink}" stroke="#fff" stroke-width="2"/>')
    a(f'<rect x="{mw_x-11}" y="{ky+14}" width="22" height="50" rx="8" fill="{blue}" stroke="#fff" stroke-width="2"/>')
    # ручки
    for x, c in zip(knob_x, (pink, blue, green, orange)):
        a(f'<circle cx="{x}" cy="{cy}" r="19" fill="#fffaf5" stroke="{c}" stroke-width="3"/>')
        a(f'<line x1="{x}" y1="{cy}" x2="{x+9}" y2="{cy-11}" stroke="{c}" stroke-width="3" stroke-linecap="round"/>')
    # кнопка
    a(f'<rect x="{btn_x-18}" y="{cy-13}" width="36" height="26" rx="7" fill="#fffaf5" stroke="{purple}" stroke-width="3"/>')
    # педаль
    a(f'<rect x="{ped_x-55}" y="{ped_y}" width="110" height="22" rx="7" fill="#3b3b4f"/>')
    a(f'<rect x="{ped_x-47}" y="{ped_y+4}" width="94" height="14" rx="5" fill="#4b4b63"/>')

    # карточки: (ключ, cc-ключ или None, цвет, x маркера, y маркера)
    items = [
        ("pitch", None,     pink,   pw_x,       ky - 12),
        ("mod",   "mod_cc", blue,   mw_x,       ky - 12),
        ("vol",   "vol_cc", pink,   knob_x[0],  ky - 12),
        ("bri",   "bri_cc", blue,   knob_x[1],  ky - 12),
        ("res",   "res_cc", green,  knob_x[2],  ky - 12),
        ("rel",   "rel_cc", orange, knob_x[3],  ky - 12),
        ("pc",    "pc_cc",  purple, btn_x,      ky - 12),
        ("sus",   "sus_cc", green,  ped_x + 78, ped_y + 11),
    ]
    # маркеры на рисунке
    for n, (key, cckey, c, mx, my) in enumerate(items, 1):
        a(f'<circle cx="{mx}" cy="{my}" r="10" fill="{c}" stroke="#fffaf5" stroke-width="2"/>')
        a(f'<text class="num" x="{mx}" y="{my+4.5}">{n}</text>')

    # карточки 4 x 2
    cw, ch, gap = 198, 124, 14
    gx, gy = 30, 296
    for n, (key, cckey, c, mx, my) in enumerate(items, 1):
        col, row = (n - 1) % 4, (n - 1) // 4
        x = gx + col * (cw + gap)
        y = gy + row * (ch + gap)
        a(f'<rect x="{x}" y="{y}" width="{cw}" height="{ch}" rx="14" fill="#ffffff" stroke="#efe6dd"/>')
        a(f'<circle cx="{x+22}" cy="{y+24}" r="10" fill="{c}"/>')
        a(f'<text class="num" x="{x+22}" y="{y+28.5}">{n}</text>')
        cc = t[cckey] if cckey else "Pitch Bend"
        w = len(cc) * 7.6 + 18
        a(f'<rect x="{x+cw-w-12:.0f}" y="{y+14}" width="{w:.0f}" height="20" rx="10" fill="{c}"/>')
        a(f'<text class="cc" x="{x+cw-w/2-12:.0f}" y="{y+28.3}">{esc(cc)}</text>')
        a(f'<text class="l" x="{x+14}" y="{y+58}">{esc(t[key])}</text>')
        for j, ln in enumerate(wrap(t[key + "_d"], 24)[:3]):
            a(f'<text class="d" x="{x+14}" y="{y+78+j*16}">{esc(ln)}</text>')
    a(f'<text class="n" x="30" y="{H-14}">{esc(t["note"])}</text>')
    a('</svg>')
    return "\n".join(o)

if __name__ == "__main__":
    import os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for lang in ("ru", "en"):
        svg = build(lang) + "\n"
        for d in ("docs", "site"):
            with open(os.path.join(root, d, f"controls_{lang}.svg"), "w", encoding="utf-8", newline="\n") as f:
                f.write(svg)
