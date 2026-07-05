# Wii Game Direct Launch — Integration Plan

## Goal
Launch Wii games (`.wbfs`) directly from this app without requiring USBLoaderGX or any
other homebrew app at runtime. The only prerequisite is d2x cIOS already installed on
the user's Wii (standard homebrew setup).

Design goals: **modern, small, fast, efficient, modular.**

---

## Prerequisites (user-side, already satisfied)
- Homebrew Channel installed
- d2x cIOS installed into slot 249 (standard)

---

## Phase 0 — Shared foundations

Define the types and contracts that all later phases depend on. Doing this first keeps
each phase genuinely independent and avoids retrofitting.

### GameshelfError — shared error type

One enum used by every module so callers never have to translate between error spaces:

```c
typedef enum {
    GS_OK = 0,
    GS_ERR_IO,           /* file/device read failure            */
    GS_ERR_NOT_FOUND,    /* game ID / file not present          */
    GS_ERR_BAD_FORMAT,   /* corrupt header / wrong magic        */
    GS_ERR_NO_MEMORY,    /* allocation failed                   */
    GS_ERR_USB,          /* USB device error                    */
    GS_ERR_IOS,          /* IOS reload / ioctl failure          */
    GS_ERR_CRYPTO,       /* decryption failure                  */
} GameshelfError;
```

### StorageHandle — storage abstraction

Wrap the raw USB storage handle so phases 2–4 don't call USB ioctls directly. Allows
testing with a file-backed stub on the host build:

```c
typedef struct StorageHandle StorageHandle;
GameshelfError storage_read(StorageHandle *h, u64 offset, u32 len, void *buf);
void           storage_close(StorageHandle *h);
```

### Memory strategy

| Region      | Range                       | Use                                              |
| ----------- | --------------------------- | ------------------------------------------------ |
| MEM1 low    | `0x80004000` – `0x817FFFFF` | App code + small stack allocations               |
| MEM2        | `0x90000000` – `0x93FFFFFF` | Large read buffers, wlba tables, sector cache    |
| ARGV region | `0x93002000`                | HBC ARGV block (already implemented)             |
| Trampoline  | `0x90100000`                | DOL loader stub (must not be overwritten by DOL) |

Allocate large buffers from MEM2 with `SYS_GetArena2Lo` / `memalign` rather than the
stack or global BSS.

### Files to create
- `source/model/error.h` — `GameshelfError` enum + `gs_error_str()`
- `source/model/storage.h` / `storage.c` — `StorageHandle`, `storage_read()`,
  `storage_close()`

---

## Phase 1 — DOL Loader

Load a `.dol` file from the filesystem into memory and execute it. Useful on its own
(replaces the current `SYS_RETURNTOMENU` / ARGV approach which doesn't work) and
required for the apploader step in Phase 4.

### DOL format
```
Header (0x100 bytes):
  u32 text_offsets[7]    — file offset of each text section
  u32 data_offsets[11]   — file offset of each data section
  u32 text_addrs[7]      — load address for each text section
  u32 data_addrs[11]     — load address for each data section
  u32 text_sizes[7]      — size of each text section
  u32 data_sizes[11]     — size of each data section
  u32 bss_addr           — start of BSS region (zero-fill)
  u32 bss_size           — size of BSS region
  u32 entry_point        — address to jump to after loading
```

### Steps
1. Open the DOL file, read the 0x100-byte header
2. For each text and data section with non-zero size: read in 32KB chunks (32-byte
   aligned) directly into the load address — chunked reads are faster with libfat than
   one large `fread`
3. `memset` the BSS region to zero
4. `DCFlushRange` + `ICInvalidateRange` over every loaded section and the BSS region
5. Write the ARGV block at `0x93002000` (already implemented correctly)
6. **Copy the trampoline stub to `0x90100000`**, then jump into it — the stub does the
   final `DCFlushRange` / `ICInvalidateRange` on itself and jumps to the entry point

### Trampoline stub (critical)
If any DOL section overlaps the address range where our app is running, copying that
section in step 2 corrupts our own code before we reach the entry point call. The
trampoline is a ~20-instruction PPC assembly function that is copied to MEM2
(`0x90100000`) before any sections are loaded. It carries out the cache flushes and
final jump — our app's code is never executed again after the trampoline is entered.

The stub is small enough to be a `static const u32[]` array of pre-assembled PPC
instructions in `dol.c`.

### Files to create
- `source/model/dol.h` — `DolHeader` struct, `dol_load_and_run(const char *path)`
- `source/model/dol.c` — implementation; returns `GameshelfError` on failure

### Testable independently
Boot any `.dol` (e.g. a second test app) by path. No USB, cIOS, or WBFS needed.

---

## Phase 2 — IOS / USB Initialisation

Switch into cIOS and bring up USB Mass Storage via raw IOS ioctls (no libwbfs
dependency). The `ios.c` file already exists; this phase adds `ios_usb_mount()`.

### Steps
1. `IOS_ReloadIOS(249)` — switch into d2x cIOS (already in `ios.c`, currently skipped)
2. Open `/dev/usb/msc` or `/dev/usb/ehc` and issue the SCSI INQUIRY ioctl to confirm
   a mass storage device is present
3. Read the MBR partition table; find a partition with WBFS magic (`"WBFS"` at offset 0
   of the partition)
4. Return a `StorageHandle` wrapping the open USB file descriptor

### Notes
- Use raw IOS ioctls (`IOS_Ioctlv`) rather than libwbfs — keeps the binary small and
  avoids a third-party dependency
- The exact ioctl sequences: reference USBLoaderGX `source/usbloader/usbstorage.c` for
  the command numbers; do not copy code
- cIOS slot is `#define`'d in `ios.h` — already configurable
- USB init can take up to ~2 s; display a "Loading…" indicator in the UI before calling
- **Startup sequencing:** `IOS_ReloadIOS` must happen before `game_library_scan()` for
  USB roots to be enumerable. `main.c` init order will need adjusting in this phase

### Files to modify
- `source/model/ios.c` — uncomment / rewrite `ios_preflight()` steps 1–4, add
  `ios_usb_mount()` returning `StorageHandle *`
- `source/main.c` — move IOS reload before `fatInitDefault()` / `game_library_scan()`

---

## Phase 3 — WBFS Reader

Read game disc sectors from a WBFS partition via a `StorageHandle`. Sparse blocks must
be handled — a wlba entry of `0` means an unallocated disc region; return zeroed sectors
rather than reading from the device.

### WBFS container format
```
Partition header (at start of WBFS partition):
  u8[4]  magic          — "WBFS"
  u32    n_hd_sec       — number of HD sectors in partition
  u8     hd_sec_sz_s    — log2 of HD sector size (usually 9 → 512 bytes)
  u8     wbfs_sec_sz_s  — log2 of WBFS block size (usually 15+ → 32 KB+)
  u8     _pad[2]
  u8     disc_table[500] — 1 byte per slot; non-zero = occupied

Each game slot:
  Block 0: wii disc header (game ID at offset 0, 6 bytes)
  wlba table: u16[] mapping Wii disc LBA → WBFS block number (0 = sparse/unallocated)
  Data blocks
```

### Steps
1. Read and validate the partition header (check magic, derive sector sizes)
2. Scan `disc_table`; for each occupied slot read block 0 to get the game ID
3. Given a target game ID, locate its slot and load its wlba table into MEM2
4. Implement `wbfs_read_sector(ctx, lba, buf)`:
   - `wbfs_block = wlba_table[lba >> (wbfs_sec_sz_s - 15)]`
   - If `wbfs_block == 0`: `memset(buf, 0, 2048)` and return (sparse block)
   - `byte_offset = (u64)wbfs_block * wbfs_sec_size + (lba & mask) * 512`
   - `storage_read(handle, byte_offset, 2048, buf)`

### Interface
```c
typedef struct WbfsContext WbfsContext;
GameshelfError wbfs_open(StorageHandle *storage, WbfsContext **ctx_out);
GameshelfError wbfs_find_game(WbfsContext *ctx, const char *id, int *slot_out);
GameshelfError wbfs_read_sector(WbfsContext *ctx, int slot, u32 lba, void *buf);
void           wbfs_close(WbfsContext *ctx);
```

### Files to create
- `source/model/wbfs.h` / `wbfs.c`

---

## Phase 4 — Game Boot

Wire everything together. The key detail not in the original plan: **retail Wii games
cannot be launched by jumping directly to `main.dol`**. The apploader must be executed
first — it maps the game's DOL segments using callbacks, applies region patches, and
returns the correct entry point.

### Disc layout recap
```
Sector 0x00000: Disc header (game ID, title, magic)
Sector 0x40000: Partition table
Each partition:
  +0x2440:  apploader.img  (executed to load the game)
  +varies:  main.dol       (loaded by apploader, not directly)
  +varies:  fst.bin        (file system table)
```

### Steps
1. Read disc sector 0 via `wbfs_read_sector` — verify Wii disc magic (`0x5D1C9EA3`)
2. Read partition table at sector `0x40000 / 2048`; find the data partition (type 0)
3. Open the partition:
   a. Read the partition header to get the title key IV (partition offset + `0x1DC`)
   b. Decrypt the title key using the Wii common key (AES-128-CBC) via the IOS
      hardware AES engine (`/dev/aes` ioctl) — faster than software AES, no extra code
4. Read `apploader.img` from partition offset `0x2440`:
   - Header: entry point, size, trailer size
   - Load body to `0x81200000` (standard apploader load address)
5. Execute the apploader's three-phase protocol:
   ```c
   apploader_init(report_fn);          // pass a print callback or NULL
   while (apploader_main(&buf, &addr, &size)) {
       wbfs_read_and_decrypt(buf, addr, size);
   }
   void (*game_entry)(void) = apploader_close();
   ```
6. Set up the cIOS USB disc read hook so in-game disc reads go through our
   `wbfs_read_sector` — reference USBLoaderGX `source/usbloader/disc.c` for the
   exact ioctl command numbers; do not copy code
7. Signal "disc inserted" to the cIOS
8. Use the DOL loader trampoline (Phase 1) to jump to `game_entry`

### Notes
- The Wii common key is public knowledge in the homebrew community
- Hardware AES via `/dev/aes` keeps binary size down vs. a software implementation
- Some games need video mode / language / region patches — out of scope for v1

### Files to create
- `source/model/disc.h` / `disc.c` — `disc_launch(WbfsContext *ctx, const char *game_id)`

---

## Integration into existing code

Once Phase 4 is complete, `pages.c` `main_select()` changes from:

```c
case CONSOLE_WII:
    launch_wii_game(index);   // current: broken ARGV approach
    break;
```

to:

```c
case CONSOLE_WII:
    disc_launch(wbfs_ctx, game->id);
    break;
```

`ConsoleInfo.loader_path` can remain as a fallback displayed in the help page for users
without cIOS, but is no longer part of the launch path.

---

## Build order

| Phase            | Depends on | Risk   | Independently testable        |
| ---------------- | ---------- | ------ | ----------------------------- |
| 0 — Foundations  | —          | None   | N/A (types only)              |
| 1 — DOL loader   | Phase 0    | Low    | Yes — boot any .dol by path   |
| 2 — IOS/USB init | Phase 0    | Medium | Yes — confirm USB mounts      |
| 3 — WBFS reader  | Phase 2    | Medium | Yes — dump sector 0 of a game |
| 4 — Game boot    | All        | High   | No — needs everything         |

Start with Phase 1. It unblocks the current USBLoaderGX launch (which currently fails
because `SYS_RETURNTOMENU` doesn't chain-launch) and builds the trampoline foundation
that Phase 4 depends on.


The simplest self-contained piece. Load a `.dol` file from the filesystem into memory
and execute it. This is useful on its own (can replace the current USBLoaderGX ARGV
approach) and is required for Phase 3.

### DOL format
```
Header (0x100 bytes):
  u32 text_offsets[7]    — file offset of each text section
  u32 data_offsets[11]   — file offset of each data section
  u32 text_addrs[7]      — load address for each text section
  u32 data_addrs[11]     — load address for each data section
  u32 text_sizes[7]      — size of each text section
  u32 data_sizes[11]     — size of each data section
  u32 bss_addr           — start of BSS region (zero-fill)
  u32 bss_size           — size of BSS region
  u32 entry_point        — address to jump to after loading
```

### Steps
1. Open the DOL file, read the 0x100-byte header
2. For each text and data section with non-zero size: `fread` into the load address
3. `memset` the BSS region to zero
4. Flush the data cache and invalidate the instruction cache over all loaded regions
   (`DCFlushRange` / `ICInvalidateRange` from libogc)
5. Cast the entry point to a function pointer and call it

### Files to create
- `source/model/dol.h` — `DolHeader` struct, `dol_load_and_run(const char *path)`
- `source/model/dol.c` — implementation

### Testable independently
Boot any `.dol` (e.g. HBC itself, or a test app) by path. No USB or WBFS needed.

---

## Phase 2 — IOS / USB Initialisation

Switch into cIOS and bring up USB Mass Storage so the Wii's disc subsystem is
redirected to the USB device.

### Steps
1. Call `IOS_ReloadIOS(249)` to switch into d2x cIOS
2. Initialise the USB storage device via `WBFS_Init` (from libwbfs, or raw IOS ioctls)
3. Open the WBFS partition — scan USB devices for a partition with the WBFS magic
4. Verify the partition is readable

### Notes
- The exact IOS ioctl sequences are best cross-referenced from USBLoaderGX's
  `source/usbloader/usbstorage.c` — use it as a reference, not a copy
- cIOS slot 249 is the most common d2x target; consider making the slot configurable
- USB initialisation can take up to ~2 seconds; display a loading indicator

### Files to create
- `source/model/ios.h` / `ios.c` — `ios_init(int slot)`, `ios_usb_mount()`

---

## Phase 3 — WBFS Reader

Read game disc sectors from the WBFS container by game ID.

### WBFS container format overview
```
Partition header:
  magic "WBFS"
  u32 n_hd_sec        — number of HD sectors in partition
  u8  hd_sec_sz_s     — log2 of HD sector size (usually 9 = 512 bytes)
  u8  wbfs_sec_sz_s   — log2 of WBFS block size (usually 15+ = 32KB+)
  disc table[500]     — 1 byte per slot, non-zero = occupied

Each game entry:
  Wii disc header at block 0 (contains game ID at offset 0)
  wlba table          — maps Wii disc LBA → WBFS block number
  Actual data blocks
```

### Steps
1. Parse the partition header, locate the disc table
2. Scan disc table slots; for each occupied slot read the disc header to get the game ID
3. Given a game ID, find its slot and load its wlba table into memory
4. Implement `wbfs_read_sector(game, lba, buf)`:
   - `block = wlba_table[lba >> (wbfs_sec_sz_s - 15)]`
   - `offset = block * wbfs_sec_size + (lba & mask) * 512`
   - Seek + read from the USB device

### Files to create
- `source/model/wbfs.h` / `wbfs.c` — `wbfs_open()`, `wbfs_find_game(id)`,
  `wbfs_read_sector()`

---

## Phase 4 — Game Boot

Wire everything together: read the game partition, decrypt it, set up IOS disc read
interception, and execute the game.

### Steps
1. Read the partition table from disc sector 0x40000 (Wii disc layout)
2. Find the data partition (type 0)
3. Decrypt the partition title key using the Wii common key (AES-128-CBC)
4. Set up the cIOS USB disc read hook — the cIOS exposes an ioctl to register your
   sector reader as the disc source
5. Signal the cIOS that the disc is "inserted" and ready
6. Load the game's `main.dol` via the DOL loader from Phase 1

### Notes
- The Wii common key is public knowledge in the homebrew community
- The cIOS disc hook ioctl sequence is the most hardware-specific part; reference
  USBLoaderGX's `source/usbloader/disc.c` for the exact commands
- Some games require additional patches (video mode, language, region) — out of scope
  for an initial implementation

### Files to create
- `source/model/disc.h` / `disc.c` — `disc_launch(const char *game_id)`

---

## Integration into existing code

Once Phase 4 is complete, `pages.c` `main_select()` changes from:

```c
case CONSOLE_WII:
    launch_wii_game(index);  // currently: boot USBLoaderGX via ARGV
    break;
```

to:

```c
case CONSOLE_WII:
    disc_launch(game->id);   // direct launch
    break;
```

The `ConsoleInfo.loader_path` field can be kept as a fallback for when cIOS is absent.

---

## Suggested build order

| Phase | Dependency | Risk |
|-------|-----------|------|
| 1 — DOL loader | None | Low |
| 2 — IOS/USB init | Phase 1 | Medium (hardware-specific) |
| 3 — WBFS reader | Phase 2 | Medium |
| 4 — Game boot | All | High (crypto + IOS hooks) |

Start with Phase 1 — it is entirely testable on-device with no USB or WBFS involvement
and builds the foundation everything else depends on.
