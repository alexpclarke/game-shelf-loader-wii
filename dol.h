#ifndef DOL_H
#define DOL_H

#include "../../model/loader.h"
#include <stdint.h>

#define DOL_MAX_TEXT    7
#define DOL_MAX_DATA    11

/*
 * DOL file header (exactly 0x100 bytes).
 * All fields are big-endian u32 — matches PPC native byte order so no
 * swapping is needed on Wii hardware.
 */
typedef struct {
    uint32_t text_offsets[DOL_MAX_TEXT];   /* file offset of each text section  */
    uint32_t data_offsets[DOL_MAX_DATA];   /* file offset of each data section  */
    uint32_t text_addrs[DOL_MAX_TEXT];     /* load address for each text section */
    uint32_t data_addrs[DOL_MAX_DATA];     /* load address for each data section */
    uint32_t text_sizes[DOL_MAX_TEXT];     /* byte size of each text section     */
    uint32_t data_sizes[DOL_MAX_DATA];     /* byte size of each data section     */
    uint32_t bss_addr;                     /* BSS region start (zero-filled)     */
    uint32_t bss_size;                     /* BSS region size in bytes           */
    uint32_t entry_point;                  /* address to jump to after loading   */
    uint8_t  _pad[0x1C];                   /* pad header to exactly 0x100 bytes  */
} DolHeader;

/*
 * Load a DOL file from `path` into memory and execute it.
 *
 * Before jumping to the entry point, sets up the HBC ARGV block at
 * 0x93002000 with the provided argc/argv (argv[0] should be the DOL path).
 * Execution transfers via a MEM2 trampoline at 0x90100000 to avoid
 * corruption if any DOL section overlaps our app's address range.
 *
 * Does not return on success.
 * Returns a GameshelfError if the file cannot be read or parsed.
 *
 * On host builds this is a no-op stub (returns GS_OK immediately).
 */
GameshelfError dol_load_and_run(const char *path, int argc, const char **argv);

#endif /* DOL_H */
