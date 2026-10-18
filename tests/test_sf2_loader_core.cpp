/**
 * Хост-тесты ядра загрузчика SoundFont-банка в QSPI Flash (Platform/qspi_xip/sf2_loader_core.c).
 * Флеш эмулируется как NOR: стирание -> 0xFF, запись только сбрасывает биты (запись в нестёртое
 * место считается нарушением), можно внедрять сбой после N-й записи страницы.
 */
#include "sf2_loader_core.h"
#include "pm_crc32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "check.h"

namespace {

constexpr uint32_t kFlashSize = 0x100000;               /* 1 МБ: те же формулы, что и для 8 МБ */
constexpr uint32_t kHdrAddr   = kFlashSize - 4096;
constexpr uint32_t kBankMax   = kHdrAddr;

struct FakeFlash {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kFlashSize, 0xFF);
    int erase_sectors = 0, erase_blocks = 0, programs = 0, nor_violations = 0;
    int fail_after_programs = -1;      /* >=0: program_page вернёт false после стольких успешных */
    bool corrupt_on_map = false;       /* map() отдаёт искажённые данные (имитация ошибки чтения XIP) */
    bool mapped = false;
    std::vector<uint32_t> erased_data_addrs;   /* адреса стираний кроме сектора заголовка */
    std::vector<uint32_t> erased_data_sizes;
    std::vector<uint8_t> corrupted;
};

bool f_read(void *c, uint32_t a, void *d, uint32_t n)
{
    auto *f = (FakeFlash *)c;
    if (a + n > kFlashSize) return false;
    std::memcpy(d, &f->mem[a], n);
    return true;
}
bool f_erase(FakeFlash *f, uint32_t a, uint32_t n)
{
    if (a % n || a + n > kFlashSize) return false;
    std::memset(&f->mem[a], 0xFF, n);
    if (a != kHdrAddr) { f->erased_data_addrs.push_back(a); f->erased_data_sizes.push_back(n); }
    return true;
}
bool f_erase_sector(void *c, uint32_t a) { auto *f = (FakeFlash *)c; ++f->erase_sectors; return f_erase(f, a, 4096); }
bool f_erase_block(void *c, uint32_t a)  { auto *f = (FakeFlash *)c; ++f->erase_blocks;  return f_erase(f, a, 65536); }
bool f_program(void *c, uint32_t a, const void *s, uint32_t n)
{
    auto *f = (FakeFlash *)c;
    if (f->fail_after_programs >= 0 && f->programs >= f->fail_after_programs) return false;
    if (n == 0 || n > 256 || (a % 256) + n > 256 || a + n > kFlashSize) { ++f->nor_violations; return false; }
    ++f->programs;
    const uint8_t *p = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; ++i) {
        if ((f->mem[a + i] & p[i]) != p[i]) ++f->nor_violations;   /* нужно 0->1 без стирания */
        f->mem[a + i] &= p[i];
    }
    return true;
}
bool f_map(void *c, const uint8_t **base)
{
    auto *f = (FakeFlash *)c;
    f->mapped = true;
    if (f->corrupt_on_map) {
        f->corrupted = f->mem;
        f->corrupted[100] ^= 0x01;
        *base = f->corrupted.data();
    } else {
        *base = f->mem.data();
    }
    return true;
}
void f_unmap(void *c) { ((FakeFlash *)c)->mapped = false; }

const SF2FlashOps kFlashOps = { kFlashSize, f_read, f_erase_sector, f_erase_block, f_program, f_map, f_unmap };

struct FakeSource {
    SF2SrcStatus open_status = SF2SRC_OK;
    std::vector<uint8_t> data;
    uint32_t stamp = 0x11112222;
    uint32_t pos = 0;
    uint64_t bytes_read = 0;
    int open_count = 0, close_count = 0;
    int fail_read_at = -1;              /* смещение, на котором чтение падает */
};
SF2SrcStatus s_open(void *c, uint32_t *size, uint32_t *stamp)
{
    auto *s = (FakeSource *)c;
    ++s->open_count; s->pos = 0;
    if (s->open_status != SF2SRC_OK) return s->open_status;
    *size = (uint32_t)s->data.size(); *stamp = s->stamp;
    return SF2SRC_OK;
}
bool s_read(void *c, void *d, uint32_t n, uint32_t *got)
{
    auto *s = (FakeSource *)c;
    if (s->fail_read_at >= 0 && s->pos + n > (uint32_t)s->fail_read_at) return false;
    uint32_t k = (uint32_t)std::min<size_t>(n, s->data.size() - s->pos);
    std::memcpy(d, &s->data[s->pos], k);
    s->pos += k; s->bytes_read += k; *got = k;
    return true;
}
bool s_rewind(void *c) { ((FakeSource *)c)->pos = 0; return true; }
void s_close(void *c) { ++((FakeSource *)c)->close_count; }
const SF2SourceOps kSrcOps = { s_open, s_read, s_rewind, s_close };

std::vector<uint8_t> make_sf2(uint32_t size, uint32_t seed)
{
    std::vector<uint8_t> v(size);
    std::srand(seed);
    for (auto &b : v) b = (uint8_t)std::rand();
    std::memcpy(&v[0], "RIFF", 4);
    uint32_t riff = size - 8;
    std::memcpy(&v[4], &riff, 4);
    std::memcpy(&v[8], "sfbk", 4);
    return v;
}

uint8_t g_buf[4096];

SF2L_Result run(FakeFlash &fl, FakeSource *src, SF2L_Report *rep)
{
    SF2L_Env env{};
    env.flash = &kFlashOps; env.flash_ctx = &fl;
    env.src = src ? &kSrcOps : nullptr; env.src_ctx = src;
    env.buf = g_buf; env.buf_size = sizeof(g_buf);
    return SF2L_Run(&env, rep);
}

void reset_counters(FakeFlash &f)
{
    f.erase_sectors = f.erase_blocks = f.programs = 0;
    f.erased_data_addrs.clear(); f.erased_data_sizes.clear();
}

void test_crc()
{
    CHECK(PM_Crc32_Update(0, "123456789", 9) == 0xCBF43926u);
    uint32_t c = PM_Crc32_Update(0, "1234", 4);
    c = PM_Crc32_Update(c, "56789", 5);
    CHECK(c == 0xCBF43926u);
}

void test_signature()
{
    uint8_t h[12];
    std::memcpy(h, "RIFF", 4); uint32_t sz = 1000; std::memcpy(h + 4, &sz, 4); std::memcpy(h + 8, "sfbk", 4);
    CHECK(SF2L_SignatureOk(h, 1008));
    CHECK(!SF2L_SignatureOk(h, 1007));           /* RIFF больше файла */
    std::memcpy(h + 8, "WAVE", 4);
    CHECK(!SF2L_SignatureOk(h, 1008));
    std::memcpy(h + 8, "sfbk", 4); std::memcpy(h, "RIFX", 4);
    CHECK(!SF2L_SignatureOk(h, 1008));
}

void test_first_flash_and_unchanged()
{
    FakeFlash fl; FakeSource src; src.data = make_sf2(300000, 1);   /* не кратно 4096/256 */
    SF2L_Report rep;

    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);
    CHECK(rep.file_status == SF2L_FILE_NEW);
    CHECK(rep.size == 300000);
    CHECK(rep.crc32 == PM_Crc32_Update(0, src.data.data(), src.data.size()));
    CHECK(std::memcmp(fl.mem.data(), src.data.data(), src.data.size()) == 0);
    CHECK(rep.base == fl.mem.data());
    CHECK(fl.nor_violations == 0);
    CHECK(fl.mapped == true);                        /* финальное состояние: XIP включён */
    CHECK(src.open_count == 1 && src.close_count == 1);
    /* 300000 Б -> 74 сектора (303104 Б): 4 блока по 64 КБ (262144) + 10 секторов, + сектор заголовка */
    CHECK(fl.erase_blocks == 4);
    CHECK(fl.erase_sectors == 10 + 1);

    /* Второй старт: тот же файл и штамп -> никаких обращений на запись, файл не читается целиком. */
    reset_counters(fl); src.bytes_read = 0;
    CHECK(run(fl, &src, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_SAME);
    CHECK(fl.erase_sectors == 0 && fl.erase_blocks == 0 && fl.programs == 0);
    CHECK(src.bytes_read <= 12 + 12);               /* только проверка сигнатуры (читалась 1 раз, до rewind) */

    /* Тот же файл, но новый штамп (скопирован заново) -> CRC совпал -> флеш не трогаем. */
    src.stamp = 0x33334444; reset_counters(fl); src.bytes_read = 0;
    CHECK(run(fl, &src, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_SAME);
    CHECK(fl.erase_sectors == 0 && fl.erase_blocks == 0 && fl.programs == 0);
    CHECK(src.bytes_read >= 300000);                 /* CRC прочитал файл целиком */

    /* Файл изменился -> перезапись. */
    src.data = make_sf2(300000, 2); reset_counters(fl);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);
    CHECK(std::memcmp(fl.mem.data(), src.data.data(), src.data.size()) == 0);
    CHECK(fl.nor_violations == 0);

    /* Файл другого размера, меньше прежнего: хвост старого банка не мешает. */
    src.data = make_sf2(70000, 3); reset_counters(fl);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);
    CHECK(rep.size == 70000);
    CHECK(std::memcmp(fl.mem.data(), src.data.data(), src.data.size()) == 0);
    CHECK(fl.nor_violations == 0);
}

void test_max_size_never_touches_header_region()
{
    FakeFlash fl; FakeSource src; src.data = make_sf2(kBankMax, 4);
    SF2L_Report rep;
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);
    CHECK(rep.size == kBankMax);
    CHECK(std::memcmp(fl.mem.data(), src.data.data(), kBankMax) == 0);
    for (size_t i = 0; i < fl.erased_data_addrs.size(); ++i) {
        CHECK(fl.erased_data_addrs[i] + fl.erased_data_sizes[i] <= kHdrAddr);   /* данные не заходят в заголовок */
    }
    CHECK(fl.nor_violations == 0);

    FakeFlash fl2; FakeSource big; big.data = make_sf2(kBankMax + 1, 5);
    CHECK(run(fl2, &big, &rep) == SF2L_ERR_NO_BANK);
    CHECK(rep.file_status == SF2L_FILE_TOO_LARGE);
    CHECK(fl2.erase_sectors == 0 && fl2.erase_blocks == 0 && fl2.programs == 0);
}

void test_invalid_files()
{
    SF2L_Report rep;
    FakeFlash fl; FakeSource good; good.data = make_sf2(100000, 6);
    CHECK(run(fl, &good, &rep) == SF2L_OK_UPDATED);

    /* Мусор вместо банка: старый банк остаётся нетронутым и используется. */
    FakeSource bad; bad.data = std::vector<uint8_t>(50000, 0x55);
    reset_counters(fl);
    CHECK(run(fl, &bad, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_BAD_SIGNATURE);
    CHECK(fl.erase_sectors == 0 && fl.erase_blocks == 0 && fl.programs == 0);
    CHECK(rep.size == 100000);

    FakeSource tiny; tiny.data = make_sf2(100, 7);
    CHECK(run(fl, &tiny, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_TOO_SMALL);

    /* Пустой флеш + мусор -> банка нет. */
    FakeFlash empty;
    CHECK(run(empty, &bad, &rep) == SF2L_ERR_NO_BANK);
    CHECK(empty.programs == 0);
}

void test_no_media_or_file()
{
    SF2L_Report rep;
    FakeFlash fl; FakeSource src; src.data = make_sf2(100000, 8);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);

    reset_counters(fl);
    src.open_status = SF2SRC_NO_MEDIA;
    CHECK(run(fl, &src, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_NO_MEDIA);
    CHECK(rep.base != nullptr && rep.size == 100000);
    CHECK(fl.erase_sectors == 0 && fl.programs == 0);

    src.open_status = SF2SRC_NO_FILE;
    CHECK(run(fl, &src, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_NO_FILE);

    CHECK(run(fl, nullptr, &rep) == SF2L_OK_UNCHANGED);   /* без источника вообще */

    FakeFlash empty;
    CHECK(run(empty, &src, &rep) == SF2L_ERR_NO_BANK);
    CHECK(run(empty, nullptr, &rep) == SF2L_ERR_NO_BANK);
}

void test_power_failure_during_write()
{
    SF2L_Report rep;
    FakeFlash fl; FakeSource src; src.data = make_sf2(200000, 9);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);          /* старый банк */

    src.data = make_sf2(200000, 10); src.stamp = 0x55556666;
    fl.fail_after_programs = 100;                           /* "питание пропало" на 100-й странице */
    reset_counters(fl);
    CHECK(run(fl, &src, &rep) == SF2L_ERR_WRITE);
    CHECK(src.close_count >= 1);

    /* Заголовок стёрт: банк не считается валидным ни при каком раскладе. */
    fl.fail_after_programs = -1; fl.programs = 0;
    FakeSource none; none.open_status = SF2SRC_NO_MEDIA;
    CHECK(run(fl, &none, &rep) == SF2L_ERR_NO_BANK);

    /* Следующий старт с картой: запись повторяется и завершается успешно. */
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);
    CHECK(std::memcmp(fl.mem.data(), src.data.data(), src.data.size()) == 0);
    CHECK(fl.nor_violations == 0);

    /* Сбой при записи самого заголовка (последняя страница). */
    FakeFlash fl2; src.data = make_sf2(50000, 11);
    fl2.fail_after_programs = (50000 + 255) / 256;          /* все страницы данных проходят, заголовок - нет */
    CHECK(run(fl2, &src, &rep) == SF2L_ERR_WRITE);
    fl2.fail_after_programs = -1;
    CHECK(run(fl2, nullptr, &rep) == SF2L_ERR_NO_BANK);
}

void test_verify_failure()
{
    SF2L_Report rep;
    FakeFlash fl; FakeSource src; src.data = make_sf2(120000, 12);
    fl.corrupt_on_map = true;                               /* XIP читает не то, что записано */
    CHECK(run(fl, &src, &rep) == SF2L_ERR_VERIFY);
    fl.corrupt_on_map = false;
    FakeSource none; none.open_status = SF2SRC_NO_MEDIA;
    CHECK(run(fl, &none, &rep) == SF2L_ERR_NO_BANK);        /* заголовок не записан */
}

void test_damaged_flash_triggers_rewrite()
{
    SF2L_Report rep;
    FakeFlash fl; FakeSource src; src.data = make_sf2(90000, 13);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);

    fl.mem[0] = 0x00;                                       /* портим сигнатуру RIFF во флеше */
    FakeSource none; none.open_status = SF2SRC_NO_FILE;
    CHECK(run(fl, &none, &rep) == SF2L_ERR_NO_BANK);

    /* Тот же файл и штамп, но банк во флеше повреждён -> обязательно переписать. */
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);
    CHECK(std::memcmp(fl.mem.data(), src.data.data(), src.data.size()) == 0);
}

void test_read_error_keeps_old_bank()
{
    SF2L_Report rep;
    FakeFlash fl; FakeSource src; src.data = make_sf2(90000, 14);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UPDATED);

    src.data = make_sf2(90000, 15); src.stamp++;
    src.fail_read_at = 40000;                               /* SD отказывает при расчёте CRC */
    reset_counters(fl);
    CHECK(run(fl, &src, &rep) == SF2L_OK_UNCHANGED);
    CHECK(rep.file_status == SF2L_FILE_READ_ERROR);
    CHECK(fl.erase_sectors == 0 && fl.erase_blocks == 0 && fl.programs == 0);
}

void test_params()
{
    FakeFlash fl; SF2L_Report rep;
    SF2L_Env env{};
    env.flash = &kFlashOps; env.flash_ctx = &fl; env.buf = g_buf; env.buf_size = 100;   /* не кратно 256 */
    CHECK(SF2L_Run(&env, &rep) == SF2L_ERR_PARAM);
    env.buf = nullptr; env.buf_size = 4096;
    CHECK(SF2L_Run(&env, &rep) == SF2L_ERR_PARAM);
}

} // namespace

int main()
{
    test_crc();
    test_signature();
    test_first_flash_and_unchanged();
    test_max_size_never_touches_header_region();
    test_invalid_files();
    test_no_media_or_file();
    test_power_failure_during_write();
    test_verify_failure();
    test_damaged_flash_triggers_rewrite();
    test_read_error_keeps_old_bank();
    test_params();

    if (purrmidi_test::Failures()) {
        std::fprintf(stderr, "%d check(s) FAILED\n", purrmidi_test::Failures());
        return 1;
    }
    std::puts("test_sf2_loader_core: all checks passed");
    return 0;
}
