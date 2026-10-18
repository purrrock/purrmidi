// Общие макросы проверок для хост-тестов (раньше каждый тест определял свои CHECK/TEST_ASSERT).
//
//   CHECK(cond)    - проверка: при ошибке печатает "FAIL файл:строка: условие", увеличивает счётчик и ПРОДОЛЖАЕТ
//                    работу (так видны все сломанные проверки за один прогон). Если перед #include "check.h"
//                    определён PURRMIDI_CHECK_FAIL_FAST, CHECK ведёт себя как REQUIRE - для линейных тестов, где
//                    дальнейший код не имеет смысла после первой ошибки.
//   REQUIRE(cond)  - то же, но при ошибке сразу завершает процесс с кодом 1 (дальнейшие проверки зависят от этой).
//   Finish("имя")  - в конце main(): печатает итог и возвращает 0 или 1 (код возврата теста для ctest).
//   Failures()     - сколько проверок не прошло к этому моменту.
//
// Макросы работают при любом NDEBUG (в отличие от assert), поэтому тесты не превращаются в пустышки в Release.
#ifndef PURRMIDI_TESTS_CHECK_H
#define PURRMIDI_TESTS_CHECK_H

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace purrmidi_test {

inline std::atomic<int> &FailureCounter()
{
    static std::atomic<int> counter{0};
    return counter;
}

inline int Failures()
{
    return FailureCounter().load();
}

inline void Fail(const char *file, int line, const char *expr, bool fatal)
{
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    FailureCounter().fetch_add(1);
    if (fatal) {
        std::exit(1);
    }
}

inline int Finish(const char *name)
{
    const int n = Failures();
    if (n != 0) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, n);
        return 1;
    }
    return 0;
}

}  // namespace purrmidi_test

#ifdef PURRMIDI_CHECK_FAIL_FAST
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) purrmidi_test::Fail(__FILE__, __LINE__, #cond, true);                \
    } while (0)
#else
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) purrmidi_test::Fail(__FILE__, __LINE__, #cond, false);               \
    } while (0)
#endif

#define REQUIRE(cond)                                                                     \
    do {                                                                                  \
        if (!(cond)) purrmidi_test::Fail(__FILE__, __LINE__, #cond, true);                \
    } while (0)

#endif /* PURRMIDI_TESTS_CHECK_H */
