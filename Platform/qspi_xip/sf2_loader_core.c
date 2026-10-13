/**
 * @file    sf2_loader_core.c
 * @brief   Реализация сценария обновления SoundFont-банка в QSPI Flash (см. sf2_loader_core.h).
 */
#include "sf2_loader_core.h"
#include "pm_crc32.h"

#include <string.h>

_Static_assert(sizeof(SF2L_Header) == 64, "SF2L_Header must be 64 bytes");

#define PROGRESS(e, ph, d, t) \
    do { if ((e)->progress) { (e)->progress((e)->progress_ctx, (ph), (d), (t)); } } while (0)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool SF2L_SignatureOk(const uint8_t head12[12], uint32_t total_size)
{
    if (memcmp(head12, "RIFF", 4) != 0 || memcmp(head12 + 8, "sfbk", 4) != 0) {
        return false;
    }
    /* Размер RIFF не включает 8 байт заголовка; он не может выходить за пределы файла. */
    const uint32_t riff = rd32(head12 + 4);
    return riff >= 4u && (uint64_t)riff + 8u <= total_size;
}

const char *SF2L_ResultName(SF2L_Result r)
{
    switch (r) {
        case SF2L_OK_UNCHANGED: return "OK (bank unchanged)";
        case SF2L_OK_UPDATED:   return "OK (bank updated from SD)";
        case SF2L_ERR_PARAM:    return "bad parameters";
        case SF2L_ERR_NO_BANK:  return "no valid bank in QSPI flash";
        case SF2L_ERR_WRITE:    return "QSPI erase/program failed";
        case SF2L_ERR_VERIFY:   return "verify after write failed";
        case SF2L_ERR_FLASH:    return "QSPI memory-mapped mode failed";
    }
    return "?";
}

const char *SF2L_FileStatusName(SF2L_FileStatus s)
{
    switch (s) {
        case SF2L_FILE_NOT_CHECKED:  return "not checked";
        case SF2L_FILE_NO_MEDIA:     return "no SD card";
        case SF2L_FILE_NO_FILE:      return "no SNDFNT.SF2 on card";
        case SF2L_FILE_READ_ERROR:   return "SD read error";
        case SF2L_FILE_TOO_SMALL:    return "file too small";
        case SF2L_FILE_TOO_LARGE:    return "file larger than QSPI capacity";
        case SF2L_FILE_BAD_SIGNATURE:return "not a valid RIFF/sfbk file";
        case SF2L_FILE_SAME:         return "same as bank in flash";
        case SF2L_FILE_NEW:          return "new file";
    }
    return "?";
}

/* ---------------------------- заголовок ------------------------------------ */

static uint32_t header_crc(const SF2L_Header *h)
{
    return PM_Crc32_Update(0, h, offsetof(SF2L_Header, header_crc32));
}

static void header_build(SF2L_Header *h, uint32_t size, uint32_t crc, uint32_t stamp)
{
    memset(h, 0xFF, sizeof(*h));
    h->magic      = SF2L_MAGIC;
    h->layout     = SF2L_LAYOUT;
    h->bank_size  = size;
    h->bank_crc32 = crc;
    h->src_stamp  = stamp;
    h->header_crc32 = header_crc(h);
}

static bool header_valid(const SF2L_Header *h, uint32_t bank_max)
{
    return h->magic == SF2L_MAGIC && h->layout == SF2L_LAYOUT &&
           h->bank_size >= SF2L_BANK_MIN_SIZE && h->bank_size <= bank_max &&
           h->header_crc32 == header_crc(h);
}

/* Банк во флеше валиден: заголовок цел и в начале данных стоит сигнатура SF2. */
static bool flash_bank_valid(const SF2L_Env *e, uint32_t bank_max, SF2L_Header *hdr)
{
    const SF2FlashOps *fl = e->flash;
    uint8_t head[12];

    if (!fl->read(e->flash_ctx, fl->flash_size - SF2L_SECTOR_SIZE, hdr, sizeof(*hdr))) {
        return false;
    }
    if (!header_valid(hdr, bank_max)) {
        return false;
    }
    if (!fl->read(e->flash_ctx, 0, head, sizeof(head))) {
        return false;
    }
    return SF2L_SignatureOk(head, hdr->bank_size);
}

/* ------------------------------ источник ----------------------------------- */

static bool read_exact(const SF2L_Env *e, void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;

    while (len) {
        uint32_t got = 0;
        if (!e->src->read(e->src_ctx, p, len, &got) || got == 0 || got > len) {
            return false;
        }
        p += got;
        len -= got;
    }
    return true;
}

static SF2L_FileStatus inspect_source(const SF2L_Env *e, uint32_t bank_max, uint32_t *size, uint32_t *stamp,
                                      bool *opened)
{
    *opened = false;
    const SF2SrcStatus st = e->src->open(e->src_ctx, size, stamp);

    if (st == SF2SRC_NO_MEDIA) return SF2L_FILE_NO_MEDIA;
    if (st == SF2SRC_NO_FILE)  return SF2L_FILE_NO_FILE;
    if (st != SF2SRC_OK)       return SF2L_FILE_READ_ERROR;
    *opened = true;

    if (*size < SF2L_BANK_MIN_SIZE) return SF2L_FILE_TOO_SMALL;
    if (*size > bank_max)           return SF2L_FILE_TOO_LARGE;

    uint8_t head[12];
    if (!read_exact(e, head, sizeof(head))) return SF2L_FILE_READ_ERROR;
    if (!SF2L_SignatureOk(head, *size))     return SF2L_FILE_BAD_SIGNATURE;
    if (!e->src->rewind(e->src_ctx))        return SF2L_FILE_READ_ERROR;
    return SF2L_FILE_NEW;   /* кандидат; SAME определяется позже */
}

static bool file_crc(const SF2L_Env *e, uint32_t size, uint32_t *crc_out)
{
    uint32_t crc = 0, done = 0;

    if (!e->src->rewind(e->src_ctx)) return false;
    while (done < size) {
        const uint32_t n = (size - done < e->buf_size) ? (size - done) : e->buf_size;
        if (!read_exact(e, e->buf, n)) return false;
        crc = PM_Crc32_Update(crc, e->buf, n);
        done += n;
        PROGRESS(e, SF2L_PHASE_CHECK, done, size);
    }
    *crc_out = crc;
    return true;
}

/* ------------------------------- запись ------------------------------------ */

static bool erase_bank_area(const SF2L_Env *e, uint32_t size)
{
    const SF2FlashOps *fl = e->flash;
    const uint32_t total = (size + SF2L_SECTOR_SIZE - 1u) & ~(SF2L_SECTOR_SIZE - 1u);
    uint32_t addr = 0;

    while (addr < total) {
        /* 64 КБ блок - только если он целиком внутри области банка (никогда не заденет заголовок). */
        if ((addr % SF2L_BLOCK64_SIZE) == 0u && addr + SF2L_BLOCK64_SIZE <= total) {
            if (!fl->erase_block64(e->flash_ctx, addr)) return false;
            addr += SF2L_BLOCK64_SIZE;
        } else {
            if (!fl->erase_sector(e->flash_ctx, addr)) return false;
            addr += SF2L_SECTOR_SIZE;
        }
        PROGRESS(e, SF2L_PHASE_ERASE, addr < total ? addr : total, total);
    }
    return true;
}

static bool program_bank(const SF2L_Env *e, uint32_t size)
{
    const SF2FlashOps *fl = e->flash;
    uint32_t addr = 0;

    if (!e->src->rewind(e->src_ctx)) return false;
    while (addr < size) {
        const uint32_t n = (size - addr < e->buf_size) ? (size - addr) : e->buf_size;
        if (!read_exact(e, e->buf, n)) return false;
        for (uint32_t off = 0; off < n; off += SF2L_PAGE_SIZE) {
            const uint32_t len = (n - off < SF2L_PAGE_SIZE) ? (n - off) : SF2L_PAGE_SIZE;
            if (!fl->program_page(e->flash_ctx, addr + off, e->buf + off, len)) return false;
        }
        addr += n;
        PROGRESS(e, SF2L_PHASE_WRITE, addr, size);
    }
    return true;
}

static SF2L_Result flash_update(const SF2L_Env *e, uint32_t size, uint32_t crc, uint32_t stamp)
{
    const SF2FlashOps *fl = e->flash;
    const uint32_t hdr_addr = fl->flash_size - SF2L_SECTOR_SIZE;
    uint8_t page[SF2L_PAGE_SIZE];
    SF2L_Header hdr;

    /* 1. Сначала делаем банк невалидным: сбой дальше -> заголовка нет -> повтор на следующем старте. */
    if (!fl->erase_sector(e->flash_ctx, hdr_addr)) return SF2L_ERR_WRITE;
    if (!erase_bank_area(e, size))                 return SF2L_ERR_WRITE;
    if (!program_bank(e, size))                    return SF2L_ERR_WRITE;

    /* 2. Проверка тем же путём, которым потом читает синтезатор (XIP + CRC32 всего банка). */
    const uint8_t *base = NULL;
    if (!fl->map(e->flash_ctx, &base) || base == NULL) return SF2L_ERR_FLASH;
    PROGRESS(e, SF2L_PHASE_VERIFY, 0, size);
    const uint32_t xip_crc = PM_Crc32_Update(0, base, size);
    PROGRESS(e, SF2L_PHASE_VERIFY, size, size);
    fl->unmap(e->flash_ctx);
    if (xip_crc != crc) return SF2L_ERR_VERIFY;

    /* 3. Коммит: заголовок записывается последним. */
    header_build(&hdr, size, crc, stamp);
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &hdr, sizeof(hdr));
    if (!fl->program_page(e->flash_ctx, hdr_addr, page, SF2L_PAGE_SIZE)) return SF2L_ERR_WRITE;
    return SF2L_OK_UPDATED;
}

/* -------------------------------- сценарий --------------------------------- */

SF2L_Result SF2L_Run(const SF2L_Env *e, SF2L_Report *rep)
{
    SF2L_Report dummy;
    if (!rep) rep = &dummy;
    memset(rep, 0, sizeof(*rep));
    rep->file_status = SF2L_FILE_NOT_CHECKED;

    if (!e || !e->flash || !e->buf || e->buf_size < SF2L_PAGE_SIZE || (e->buf_size % SF2L_PAGE_SIZE) != 0u ||
        e->flash->flash_size < 4u * SF2L_SECTOR_SIZE) {
        return rep->result = SF2L_ERR_PARAM;
    }

    const SF2FlashOps *fl = e->flash;
    const uint32_t bank_max = fl->flash_size - SF2L_SECTOR_SIZE;
    SF2L_Header hdr;
    bool have_bank = flash_bank_valid(e, bank_max, &hdr);
    SF2L_Result res = SF2L_OK_UNCHANGED;

    if (e->src) {
        uint32_t fsize = 0, fstamp = 0, fcrc = 0;
        bool opened = false;
        rep->file_status = inspect_source(e, bank_max, &fsize, &fstamp, &opened);

        if (rep->file_status == SF2L_FILE_NEW) {
            bool same = false;

            if (have_bank && hdr.bank_size == fsize) {
                if (hdr.src_stamp == fstamp) {
                    same = true;                       /* тот же размер и тот же штамп: CRC не нужен */
                    fcrc = hdr.bank_crc32;
                } else if (file_crc(e, fsize, &fcrc)) {
                    same = (fcrc == hdr.bank_crc32);   /* файл скопирован заново, содержимое то же */
                } else {
                    rep->file_status = SF2L_FILE_READ_ERROR;
                }
            } else if (!file_crc(e, fsize, &fcrc)) {
                rep->file_status = SF2L_FILE_READ_ERROR;
            }

            if (rep->file_status == SF2L_FILE_NEW) {
                if (same) {
                    rep->file_status = SF2L_FILE_SAME;
                } else {
                    res = flash_update(e, fsize, fcrc, fstamp);
                    if (res != SF2L_OK_UPDATED) {
                        if (opened) e->src->close(e->src_ctx);
                        return rep->result = res;
                    }
                }
            }
        }
        if (opened) e->src->close(e->src_ctx);
    }

    /* Итог: во флеше должен быть валидный банк. Включаем XIP. */
    have_bank = flash_bank_valid(e, bank_max, &hdr);
    if (!have_bank) {
        return rep->result = SF2L_ERR_NO_BANK;
    }
    const uint8_t *base = NULL;
    if (!fl->map(e->flash_ctx, &base) || base == NULL) {
        return rep->result = SF2L_ERR_FLASH;
    }
    rep->base  = base;
    rep->size  = hdr.bank_size;
    rep->crc32 = hdr.bank_crc32;
    return rep->result = (res == SF2L_OK_UPDATED) ? SF2L_OK_UPDATED : SF2L_OK_UNCHANGED;
}
