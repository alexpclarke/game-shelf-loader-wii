# Wii Loader

A Wii-native game launcher module for the [game-shelf](https://github.com/alexpclarke/game-shelf) project. Implements the `Loader` interface to find, read, and boot Wii disc images stored on a USB drive in WBFS format.

## How It Works

Launch sequence:

1. **cIOS preflight** (`ios.c`) — verifies cIOS slot 249 is installed and reloads into it
2. **USB mount** (`ios.c`) — opens USB storage and locates the WBFS partition
3. **WBFS open** (`wbfs.c`) — validates the partition header and builds the disc slot table
4. **Game lookup** (`wbfs.c`) — scans slots for the target game by 6-character disc ID (e.g. `RMCE01`)
5. **Sector reads** (`wbfs.c`) — reads 2048-byte disc sectors via the wlba table; sparse blocks return zeroed memory
6. **Launch** (`wii_loader.c`) — drives the above steps lazily on first launch, then boots the game

Init is deferred until launch so the game shelf menu appears immediately on startup without waiting for USB.

## Dependencies

- [devkitPPC](https://devkitpro.org/) with the `wii-dev` group installed
- [libogc](https://github.com/devkitPro/libogc) (`gccore.h`, `usbstorage.h`)
- cIOS rev21 or later installed in slot 249 on the target Wii (e.g. via d2x)

## Usage as a Submodule

```bash
git submodule add https://github.com/alexpclarke/game-shelf-loader-wii.git source/loaders/wii
```

When cloning a project that includes this submodule:

```bash
git clone --recurse-submodules <parent-repo-url>
```

Or if already cloned:

```bash
git submodule update --init
```

The module expects the parent project to provide `../../model/loader.h` (the `Loader` interface and `GameshelfError` type) and `../../log.h` at the relative paths used by the includes.
