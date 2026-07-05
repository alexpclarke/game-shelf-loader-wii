#include "dol.h"
#include "../../log.h"
#include <stdio.h>
#include <string.h>

#ifdef __wii__

#include <gccore.h>

/* ------------------------------------------------------------------ */
/* Memory map constants                                                 */
/* ------------------------------------------------------------------ */

/*
 * Trampoline location in MEM2. Must not be overwritten by any DOL section.
 * USBLoaderGX and most homebrew DOLs load into MEM1 (0x80xxxxxx), so
 * 0x90100000 in MEM2 is reliably safe.
 */
#define TRAMPOLINE_ADDR 0x90100000u

/* ARGV_MAGIC (0x5f617267 "_arg") is defined in libogc's gctypes.h via gccore.h */
#define ARGV_BASE     0x93002000u
#define ARGV_MAX      16
#define ARGV_PTRS_OFF (sizeof(HBCArgv))
#define ARGV_CMD_OFF  (ARGV_PTRS_OFF + (ARGV_MAX + 1) * sizeof(char *))
#define ARGV_CMD_SIZE (0x1000u - ARGV_CMD_OFF)

/* ------------------------------------------------------------------ */
/* HBC ARGV struct (matches struct __argv from <args.h>)               */
/* ------------------------------------------------------------------ */
typedef struct {
    int    argvMagic;
    char  *commandLine;
    int    length;
    int    argc;
    char **argv;
    char **endARGV;
} HBCArgv;

/* ------------------------------------------------------------------ */
/* MEM2 trampoline stub                                                 */
/* ------------------------------------------------------------------ */
/*
 * Two PPC instructions copied to TRAMPOLINE_ADDR at runtime:
 *   mtctr r3   — load count register from r3 (first argument = entry point)
 *   bctr       — branch to count register
 *
 * The stub is called as: trampoline(entry_point)
 * After this call our app's code is never executed again.
 */
static const uint32_t trampoline_code[] = {
    0x7C6903A6u, /* mtctr r3 */
    0x4E800420u, /* bctr     */
};

/* ------------------------------------------------------------------ */
/* ARGV setup                                                           */
/* ------------------------------------------------------------------ */
static void setup_argv(int argc, const char **args)
{
    if (argc <= 0 || !args) return;
    if (argc > ARGV_MAX) argc = ARGV_MAX;

    HBCArgv *block   = (HBCArgv *)ARGV_BASE;
    char   **ptrs    = (char **)(ARGV_BASE + ARGV_PTRS_OFF);
    char    *cmdline = (char  *)(ARGV_BASE + ARGV_CMD_OFF);

    int offset = 0;
    for (int i = 0; i < argc; i++) {
        int len = (int)strlen(args[i]) + 1;
        if (offset + len > (int)ARGV_CMD_SIZE) {
            LOG_WARN("dol: ARGV cmdline overflow at arg %d, truncating", i);
            argc = i;
            break;
        }
        memcpy(cmdline + offset, args[i], len);
        ptrs[i] = cmdline + offset;
        LOG_DEBUG("dol: argv[%d] = \"%s\"", i, ptrs[i]);
        offset += len;
    }
    ptrs[argc] = NULL;

    block->argvMagic   = (int)ARGV_MAGIC;
    block->commandLine = cmdline;
    block->length      = offset;
    block->argc        = argc;
    block->argv        = ptrs;
    block->endARGV     = ptrs + argc;

    DCFlushRange((void *)ARGV_BASE, ARGV_CMD_OFF + (uint32_t)offset);
    LOG_INFO("dol: ARGV ready: argc=%d cmdline_len=%d", argc, offset);
}

/* ------------------------------------------------------------------ */
/* Section reader                                                       */
/* ------------------------------------------------------------------ */
static GameshelfError load_section(FILE *f, uint32_t file_off,
                                   void *load_addr, uint32_t size)
{
    if (fseek(f, (long)file_off, SEEK_SET) != 0) return GS_ERR_IO;

    uint8_t *dst = (uint8_t *)load_addr;
    uint32_t remaining = size;
    while (remaining > 0) {
        uint32_t chunk = remaining < 32768u ? remaining : 32768u;
        if (fread(dst, 1, chunk, f) != chunk) return GS_ERR_IO;
        dst       += chunk;
        remaining -= chunk;
    }
    return GS_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
GameshelfError dol_load_and_run(const char *path, int argc, const char **argv)
{
    LOG_INFO("dol: loading \"%s\"", path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("dol: cannot open \"%s\"", path);
        return GS_ERR_IO;
    }

    DolHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        LOG_ERROR("dol: failed to read header");
        fclose(f);
        return GS_ERR_IO;
    }
    LOG_INFO("dol: header ok — entry=0x%08x bss=0x%08x+0x%x",
             hdr.entry_point, hdr.bss_addr, hdr.bss_size);

    /*
     * Install the trampoline in MEM2 BEFORE copying any sections.
     * If a section overlaps our app's code, copying it will corrupt us —
     * but by the time we jump into the trampoline we're done with our own code.
     */
    void *tramp = (void *)TRAMPOLINE_ADDR;
    memcpy(tramp, trampoline_code, sizeof(trampoline_code));
    DCFlushRange(tramp, sizeof(trampoline_code));
    ICInvalidateRange(tramp, sizeof(trampoline_code));
    LOG_DEBUG("dol: trampoline installed at 0x%08x", TRAMPOLINE_ADDR);

    /* Load text sections */
    for (int i = 0; i < DOL_MAX_TEXT; i++) {
        if (hdr.text_sizes[i] == 0) continue;
        LOG_DEBUG("dol: text[%d] file=0x%05x -> 0x%08x (%u bytes)",
                  i, hdr.text_offsets[i], hdr.text_addrs[i], hdr.text_sizes[i]);
        GameshelfError e = load_section(f, hdr.text_offsets[i],
                                        (void *)hdr.text_addrs[i],
                                        hdr.text_sizes[i]);
        if (e != GS_OK) {
            LOG_ERROR("dol: failed to load text section %d", i);
            fclose(f);
            return e;
        }
    }

    /* Load data sections */
    for (int i = 0; i < DOL_MAX_DATA; i++) {
        if (hdr.data_sizes[i] == 0) continue;
        LOG_DEBUG("dol: data[%d] file=0x%05x -> 0x%08x (%u bytes)",
                  i, hdr.data_offsets[i], hdr.data_addrs[i], hdr.data_sizes[i]);
        GameshelfError e = load_section(f, hdr.data_offsets[i],
                                        (void *)hdr.data_addrs[i],
                                        hdr.data_sizes[i]);
        if (e != GS_OK) {
            LOG_ERROR("dol: failed to load data section %d", i);
            fclose(f);
            return e;
        }
    }

    fclose(f);
    LOG_INFO("dol: all sections loaded");

    /* Zero BSS */
    if (hdr.bss_size > 0) {
        memset((void *)hdr.bss_addr, 0, hdr.bss_size);
        LOG_DEBUG("dol: BSS zeroed 0x%08x+0x%x", hdr.bss_addr, hdr.bss_size);
    }

    /* Flush data cache and invalidate instruction cache for all loaded regions */
    for (int i = 0; i < DOL_MAX_TEXT; i++) {
        if (hdr.text_sizes[i] == 0) continue;
        DCFlushRange((void *)hdr.text_addrs[i], hdr.text_sizes[i]);
        ICInvalidateRange((void *)hdr.text_addrs[i], hdr.text_sizes[i]);
    }
    for (int i = 0; i < DOL_MAX_DATA; i++) {
        if (hdr.data_sizes[i] == 0) continue;
        DCFlushRange((void *)hdr.data_addrs[i], hdr.data_sizes[i]);
        ICInvalidateRange((void *)hdr.data_addrs[i], hdr.data_sizes[i]);
    }
    if (hdr.bss_size > 0) {
        DCFlushRange((void *)hdr.bss_addr, hdr.bss_size);
        ICInvalidateRange((void *)hdr.bss_addr, hdr.bss_size);
    }

    /* Set up HBC ARGV block */
    setup_argv(argc, argv);

    /* Transfer control via trampoline */
    LOG_INFO("dol: jumping to entry 0x%08x via trampoline at 0x%08x",
             hdr.entry_point, TRAMPOLINE_ADDR);
    log_close(); /* flush log to SD before we lose execution context */

    typedef void (*trampoline_fn_t)(void (*entry)(void));
    trampoline_fn_t trampoline_fn = (trampoline_fn_t)tramp;
    trampoline_fn((void (*)(void))hdr.entry_point);

    return GS_OK; /* unreachable */
}

#else /* host build stub */

GameshelfError dol_load_and_run(const char *path, int argc, const char **argv)
{
    (void)argc; (void)argv;
    LOG_INFO("dol: dol_load_and_run(\"%s\") — no-op on host", path);
    return GS_OK;
}

#endif /* __wii__ */
