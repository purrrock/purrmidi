#!/usr/bin/env python3
"""Генератор Platform/display/splash_logo.h: логотип для заставки дисплея 160x80 (RGB565).

Использование (из корня репозитория, нужен Pillow: pip install pillow):
    python3 tools/gen_splash_logo.py

Логотип logo/PurrMIDI_03_candy_keys.png (чёрный кот на белом фоне) уменьшается до высоты LOGO_H. Если в файле
есть прозрачность, он сначала накладывается на фон заставки BG в полном размере (край получается без тёмной каймы).
Цвет фона записывается в сгенерированный заголовок (SPLASH_BG_*), display.c берёт его оттуда.
"""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "logo" / "PurrMIDI_03_candy_keys.png"
DST = ROOT / "Platform" / "display" / "splash_logo.h"

LOGO_H = 72                    # высота логотипа на экране, пикселей (экран 80)
BG = (255, 255, 255)           # фон заставки (совпадает с фоном логотипа)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main():
    src = Image.open(SRC).convert("RGBA")
    bg = Image.new("RGBA", src.size, BG + (255,))
    flat = Image.alpha_composite(bg, src).convert("RGB")

    w = round(src.width * LOGO_H / src.height)
    img = flat.resize((w, LOGO_H), Image.LANCZOS)

    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    lines = []
    for y in range(LOGO_H):
        row = ", ".join("0x%04X" % rgb565(*px[y * w + x]) for x in range(w))
        lines.append("    " + row + ",")

    DST.write_text(
        "/* Создано tools/gen_splash_logo.py из logo/PurrMIDI_03_candy_keys.png. Вручную не править. */\n"
        "#ifndef SPLASH_LOGO_DATA_H\n#define SPLASH_LOGO_DATA_H\n\n#include <stdint.h>\n\n"
        "#define SPLASH_BG_R %d\n#define SPLASH_BG_G %d\n#define SPLASH_BG_B %d\n\n"
        "#define SPLASH_LOGO_W %d\n#define SPLASH_LOGO_H %d\n\n"
        "/* RGB565, строки сверху вниз; цвета уже смешаны со светлым фоном заставки. */\n"
        "static const uint16_t splash_logo[SPLASH_LOGO_W * SPLASH_LOGO_H] =\n{\n%s\n};\n\n#endif /* SPLASH_LOGO_DATA_H */\n"
        % (BG[0], BG[1], BG[2], w, LOGO_H, "\n".join(lines))
    )
    print("wrote %s: %dx%d, %d bytes of data" % (DST.relative_to(ROOT), w, LOGO_H, w * LOGO_H * 2))
    img.save("/tmp/splash_logo_preview.png")


if __name__ == "__main__":
    main()
