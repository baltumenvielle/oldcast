/**
 * @file hosts.h
 * @brief PCs this 3DS has connected to, saved on the SD card.
 *
 * Each entry keeps the pairing key the PC handed over on the local network, and the
 * public address the PC reported (when its router port is open). That is what lets the
 * 3DS connect, signed, from outside the home network, where discovery cannot find the PC.
 * Stored in DATA_DIR/hosts.bin.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

#define MAX_SAVED_HOSTS 8

typedef struct {
    u8 host_id[8];
    char name[33];
    u32 lan_addr;     /**< last address on the home network, network byte order */
    u32 public_addr;  /**< address from outside, network byte order; 0 if unknown */
    u16 public_port;
    bool has_key;
    u8 key[16];
} SavedHost;

/** Loads the saved PCs (no-op after the first call). */
void hosts_load(void);

int hosts_count(void);
const SavedHost *hosts_get(int index);

/** The saved entry for a PC, or NULL. */
const SavedHost *hosts_find(const u8 host_id[8]);

/** Adds or updates an entry and writes the file. */
void hosts_save(const SavedHost *host);

/** Drops the key of a PC that no longer knows this 3DS (it will pair again). */
void hosts_forget_key(const u8 host_id[8]);
