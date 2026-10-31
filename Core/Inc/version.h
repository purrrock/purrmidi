#ifndef PURRMIDI_VERSION_H
#define PURRMIDI_VERSION_H

/**
 * @file    version.h
 * @brief   Номер версии прошивки: единственное место, где он записан.
 *
 * Показывается на заставке дисплея при включении и печатается в UART при старте.
 * Формат MAJOR.MINOR.PATCH, без префикса "v" (префикс добавляется при выводе).
 *
 * Перед выпуском релиза поднимите номер здесь и создайте тег с тем же номером (git tag v1.0.3):
 * workflow .github/workflows/release.yml сверяет тег с этим файлом и не публикует релиз при расхождении.
 * Для служебных сборок можно переопределить строку ключом компилятора: -DPURRMIDI_VERSION_STRING=\"1.0.3-dev\".
 */

#define PURRMIDI_VERSION_MAJOR  1
#define PURRMIDI_VERSION_MINOR  0
#define PURRMIDI_VERSION_PATCH  4

#define PURRMIDI_STR_(x)  #x
#define PURRMIDI_STR(x)   PURRMIDI_STR_(x)

#ifndef PURRMIDI_VERSION_STRING
#define PURRMIDI_VERSION_STRING                       \
    PURRMIDI_STR(PURRMIDI_VERSION_MAJOR) "."          \
    PURRMIDI_STR(PURRMIDI_VERSION_MINOR) "."          \
    PURRMIDI_STR(PURRMIDI_VERSION_PATCH)
#endif

#endif /* PURRMIDI_VERSION_H */
