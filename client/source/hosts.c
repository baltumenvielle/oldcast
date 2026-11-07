#include "hosts.h"

#include <stdio.h>
#include <string.h>

#include "common.h"

#define HOSTS_PATH DATA_DIR "/hosts.bin"
#define FILE_MAGIC "OCH1"

static SavedHost s_hosts[MAX_SAVED_HOSTS];
static int s_count;
static bool s_loaded;

static void write_file(void)
{
    ensure_data_dir();
    FILE *f = fopen(HOSTS_PATH, "wb");
    if (!f)
        return;
    fwrite(FILE_MAGIC, 1, 4, f);
    fwrite(&s_count, sizeof(s_count), 1, f);
    fwrite(s_hosts, sizeof(SavedHost), s_count, f);
    fclose(f);
}

void hosts_load(void)
{
    if (s_loaded)
        return;
    s_loaded = true;
    s_count = 0;
    FILE *f = fopen(HOSTS_PATH, "rb");
    if (!f)
        return;
    char magic[4];
    int count = 0;
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, FILE_MAGIC, 4) == 0 && fread(&count, sizeof(count), 1, f) == 1 &&
        count > 0 && count <= MAX_SAVED_HOSTS && fread(s_hosts, sizeof(SavedHost), count, f) == (size_t)count)
        s_count = count;
    fclose(f);
}

int hosts_count(void)
{
    hosts_load();
    return s_count;
}

const SavedHost *hosts_get(int index)
{
    hosts_load();
    return index >= 0 && index < s_count ? &s_hosts[index] : NULL;
}

static SavedHost *find(const u8 host_id[8])
{
    for (int i = 0; i < s_count; i++)
        if (memcmp(s_hosts[i].host_id, host_id, 8) == 0)
            return &s_hosts[i];
    return NULL;
}

const SavedHost *hosts_find(const u8 host_id[8])
{
    hosts_load();
    return find(host_id);
}

void hosts_save(const SavedHost *host)
{
    hosts_load();
    SavedHost *entry = find(host->host_id);
    if (!entry) {
        if (s_count == MAX_SAVED_HOSTS)
            s_count--; // full: replace the last one
        entry = &s_hosts[s_count++];
    }
    *entry = *host;
    entry->name[32] = 0;
    write_file();
}

void hosts_forget_key(const u8 host_id[8])
{
    hosts_load();
    SavedHost *entry = find(host_id);
    if (entry && entry->has_key) {
        entry->has_key = false;
        memset(entry->key, 0, sizeof(entry->key));
        write_file();
    }
}
