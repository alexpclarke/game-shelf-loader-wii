#ifndef WII_LOADER_H
#define WII_LOADER_H

#include "../../model/loader.h"

/*
 * Wii direct-launch implementation of the Loader interface.
 * Finds the game in the WBFS partition, validates the disc header,
 * and (Phase 4) executes the apploader to boot the game.
 */
extern const Loader wii_loader;

#endif /* WII_LOADER_H */
