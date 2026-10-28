/* SPDX-License-Identifier: GPL-3.0-only */
/* The catalog and its mirror on the SD card: /sdcard/fm1/hub keeps the Hub's catalog.json, fm1-dock's
 * release (fm1-dock.json) and every package fetched (by its name on the Hub / GitHub), so the
 * firmwares are there without WiFi, checked the same way (size and SHA-256 against the catalog). */
#include "catalog.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "net.h"

static const char *TAG = "catalog";

#define HUB "https://thefirmwarehub.com/"
#define DOCK_RELEASE "https://api.github.com/repos/joakimeriksson/fm1-dock/releases/latest"
#define PKG_MAX (1024 * 1024)   /* the FM-1 has 1 MB of flash */
#define CARD_FM1 BOARD_SD_MOUNT_POINT "/fm1"
#define MIRROR CARD_FM1 "/hub"
#define HUB_CACHE MIRROR "/catalog.json"
#define DOCK_CACHE MIRROR "/fm1-dock.json"
/* M-VAVE's FM-1 V15 (FM-1.fwsc), the digest Felucca and the Hub pin: its USR file does not pass the
 * CRC check the others do, so it is taken by its digest alone */
#define STOCK_V15_SHA256 "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"

fw_entry_t *catalog;
int catalog_n;
char catalog_updated[32];

static void *ps_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

static void copy(char *dst, size_t len, const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    snprintf(dst, len, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static fw_entry_t *add(fw_source_t src)
{
    if (!catalog) catalog = heap_caps_calloc(CATALOG_MAX, sizeof *catalog, MALLOC_CAP_SPIRAM);
    if (!catalog || catalog_n >= CATALOG_MAX) return NULL;
    fw_entry_t *e = &catalog[catalog_n++];
    memset(e, 0, sizeof *e);
    e->src = src;
    return e;
}

static void drop(fw_source_t src)
{
    int k = 0;
    for (int i = 0; i < catalog_n; i++)
        if (catalog[i].src != src) catalog[k++] = catalog[i];
    catalog_n = k;
}

static int by_name(const void *a, const void *b)
{
    const fw_entry_t *x = a, *y = b;
    if (x->src != y->src) return x->src - y->src;
    return strcasecmp(x->name, y->name);
}

/* ---------------------------------------------------------------------------------- the card --- */

static bool sd_mounted;
static sdmmc_card_t *sd_card;

/* at boot and on a refresh: a card put in later, or one that did not start */
static bool sd_try_mount(void)
{
    for (int i = 0; i < 3 && !sd_mounted; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(250));
        esp_err_t r = board_sd_mount(BOARD_SD_MOUNT_POINT, &sd_card);
        sd_mounted = r == ESP_OK;
        if (sd_mounted) {
            ESP_LOGI(TAG, "SD card: %s, %llu MB", sd_card->cid.name,
                     (unsigned long long)sd_card->csd.capacity * sd_card->csd.sector_size / (1024 * 1024));
        } else {
            ESP_LOGW(TAG, "SD card: %s (try %d)", esp_err_to_name(r), i + 1);
        }
    }
    return sd_mounted;
}

static bool sd_mount(void) { return sd_mounted; }

bool catalog_card(void) { return sd_mounted; }

static int read_file(const char *path, uint8_t **out, size_t max);
int catalog_card_file(const char *name, uint8_t **out, size_t max)
{
    char path[300];
    if (!sd_mount()) return -1;
    snprintf(path, sizeof path, CARD_FM1 "/%s", name);
    return read_file(path, out, max);
}

static int read_file(const char *path, uint8_t **out, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > max || !(*out = heap_caps_malloc(n, MALLOC_CAP_SPIRAM))) {
        fclose(f);
        return -1;
    }
    size_t got = fread(*out, 1, n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(*out);
        return -1;
    }
    return n;
}

/* to path through path.tmp, so a card pulled mid-write leaves no half package under the real name */
static bool write_file(const char *path, const uint8_t *buf, size_t n)
{
    char tmp[sizeof ((fw_entry_t *)0)->local + 4];
    if (!sd_mount()) return false;
    char dir[sizeof tmp];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    mkdir(CARD_FM1, 0775);
    mkdir(dir, 0775);   /* the mirror's folder (hub/, modules/) */
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s", tmp);
        return false;
    }
    bool ok = fwrite(buf, 1, n, f) == n;
    ok &= fclose(f) == 0;
    unlink(path);   /* FAT's rename does not replace */
    ok = ok && rename(tmp, path) == 0;
    if (!ok) {
        unlink(tmp);
        ESP_LOGW(TAG, "writing %s failed", path);
    }
    return ok;
}

/* where an entry's package lives on the card: its file name on the server */
static void set_local(fw_entry_t *e)
{
    const char *base = strrchr(e->url, '/');
    base = base ? base + 1 : e->url;
    if (!*base || strlen(MIRROR) + 1 + strlen(base) >= sizeof e->local) {
        e->local[0] = 0;
        return;
    }
    strcpy(e->local, MIRROR "/");
    strcat(e->local, base);
}

static void mark_on_card(void)
{
    struct stat st;
    int on = 0, missing = 0, wrong = 0;
    for (int i = 0; i < catalog_n; i++) {
        fw_entry_t *e = &catalog[i];
        if (e->src == SRC_SD) continue;
        bool there = sd_mounted && e->local[0] && stat(e->local, &st) == 0;
        e->on_card = there && (!e->size || (uint32_t)st.st_size == e->size);
        on += e->on_card;
        if (!sd_mounted) continue;
        if (!there) missing++;
        else if (!e->on_card) {
            wrong++;
            ESP_LOGW(TAG, "%s: on the card %ld bytes, the catalog says %lu", e->local, (long)st.st_size, (unsigned long)e->size);
        }
    }
    if (!sd_mounted) return;
    /* what the mirror folder holds, to tell a lost file from one never saved */
    int files = 0, tmps = 0;
    long bytes = 0;
    DIR *d = opendir(MIRROR);
    for (struct dirent *de; d && (de = readdir(d));) {
        char path[300];
        snprintf(path, sizeof path, MIRROR "/%s", de->d_name);
        if (stat(path, &st) != 0) continue;
        files++;
        bytes += st.st_size;
        size_t l = strlen(de->d_name);
        if (l > 4 && strcmp(de->d_name + l - 4, ".tmp") == 0) {   /* a save cut off: never a whole package */
            tmps++;
            ESP_LOGW(TAG, "left over from a cut-off save, deleted: %s (%ld bytes)", de->d_name, (long)st.st_size);
            unlink(path);
        }
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "the mirror: %d files, %ld KB (%d .tmp); %d firmwares on the card, %d not there, %d with another size",
             files, bytes / 1024, tmps, on, missing, wrong);
}

/* the source's JSON from the net, kept on the card, or the card's when the net is not there */
static cJSON *get_json(const char *url, const char *cache, size_t max, bool *from_card, char *err, size_t errlen)
{
    uint8_t *buf;
    *from_card = false;
    int n = net_state() == NET_UP ? net_get(url, &buf, max, NULL, err, errlen) : -1;
    if (n >= 0) {
        cJSON *root = cJSON_ParseWithLength((const char *)buf, n);
        if (root && sd_mount()) write_file(cache, buf, n);
        free(buf);
        if (root) return root;
        snprintf(err, errlen, "%s does not parse", url);
    }
    if (net_state() != NET_UP) snprintf(err, errlen, "no WiFi");
    if (!sd_mount() || (n = read_file(cache, &buf, max)) < 0) return NULL;
    cJSON *root = cJSON_ParseWithLength((const char *)buf, n);
    free(buf);
    *from_card = root != NULL;
    return root;
}

/* --------------------------------------------------------------------------------- the sources --- */

static bool load_hub(bool *offline, char *err, size_t errlen)
{
    cJSON *root = get_json(HUB "catalog.json", HUB_CACHE, 1024 * 1024, offline, err, errlen);
    if (!root) return false;
    drop(SRC_HUB);
    drop(SRC_STOCK);
    copy(catalog_updated, sizeof catalog_updated, root, "updated");
    int skipped = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(root, "firmwares")) {
        const cJSON *loader = cJSON_GetObjectItemCaseSensitive(f, "loader");
        const cJSON *install = cJSON_GetObjectItemCaseSensitive(f, "install");
        const cJSON *pkg = cJSON_GetObjectItemCaseSensitive(f, "pkg");
        const cJSON *product = cJSON_GetObjectItemCaseSensitive(f, "product");
        /* M-VAVE's loader (needs M-VAVE's updater) and firmwares with their own installer are left to the Hub */
        if (!cJSON_IsNull(loader) || !cJSON_IsNull(install) || !cJSON_IsString(pkg) || !cJSON_IsString(product) ||
            strncmp(product->valuestring, "FM-1_", 5) != 0) {
            skipped++;
            continue;
        }
        fw_entry_t *e = add(SRC_HUB);
        if (!e) break;
        copy(e->id, sizeof e->id, f, "id");
        copy(e->name, sizeof e->name, f, "name");
        copy(e->version, sizeof e->version, f, "version");
        copy(e->author, sizeof e->author, f, "author");
        copy(e->kind, sizeof e->kind, f, "kind");
        copy(e->lic, sizeof e->lic, f, "lic");
        copy(e->product, sizeof e->product, f, "product");
        copy(e->ident, sizeof e->ident, f, "ident");
        copy(e->what, sizeof e->what, f, "what");
        copy(e->sha256, sizeof e->sha256, f, "sha256");
        snprintf(e->url, sizeof e->url, HUB "%s", pkg->valuestring);
        set_local(e);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(f, "size");
        e->size = cJSON_IsNumber(size) ? size->valuedouble : 0;
        e->beta = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "beta"));
    }
    const cJSON *stock = cJSON_GetObjectItemCaseSensitive(root, "stock");
    fw_entry_t *e;
    if (cJSON_IsObject(stock) && (e = add(SRC_STOCK))) {
        snprintf(e->id, sizeof e->id, "stock");
        snprintf(e->name, sizeof e->name, "Official FM-1");
        snprintf(e->author, sizeof e->author, "M-VAVE");
        snprintf(e->lic, sizeof e->lic, "Apache-2.0");
        copy(e->product, sizeof e->product, stock, "product");
        snprintf(e->version, sizeof e->version, "V15 (%.20s)", e->product);
        snprintf(e->ident, sizeof e->ident, "%s", e->product);
        snprintf(e->what, sizeof e->what, "M-VAVE's own firmware, unmodified, as the Hub keeps it: back to the FM-1 as it came.");
        copy(e->sha256, sizeof e->sha256, stock, "sha256");
        const cJSON *pkg = cJSON_GetObjectItemCaseSensitive(stock, "pkg");
        snprintf(e->url, sizeof e->url, HUB "%s", cJSON_IsString(pkg) ? pkg->valuestring : "");
        set_local(e);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(stock, "size");
        e->size = cJSON_IsNumber(size) ? size->valuedouble : 0;
        if (strcmp(e->sha256, STOCK_V15_SHA256) != 0) {   /* only the V15 we know */
            ESP_LOGW(TAG, "the Hub's stock firmware is not the V15 pinned here: left out");
            catalog_n--;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "the Hub%s: %d firmwares (%d left to the Hub's own page)", *offline ? " (from the card)" : "",
             catalog_n, skipped);
    return true;
}

/* fm1-dock's latest release: its FM-1 package (not the SMK-37's) */
static void load_dock(void)
{
    char err[64];
    bool from_card;
    cJSON *root = get_json(DOCK_RELEASE, DOCK_CACHE, 256 * 1024, &from_card, err, sizeof err);
    if (!root) {
        ESP_LOGW(TAG, "fm1-dock's release: %s", err);
        return;
    }
    drop(SRC_DOCK);
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(root, "assets")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (!cJSON_IsString(name) || !strstr(name->valuestring, ".fwsc") || strcasestr(name->valuestring, "smk")) continue;
        fw_entry_t *e = add(SRC_DOCK);
        if (!e) break;
        snprintf(e->id, sizeof e->id, "fm1-dock");
        snprintf(e->name, sizeof e->name, "fm1-dock");
        snprintf(e->author, sizeof e->author, "joakimeriksson");
        snprintf(e->lic, sizeof e->lic, "GPL-3.0");
        snprintf(e->kind, sizeof e->kind, "platform");
        copy(e->version, sizeof e->version, root, "tag_name");
        copy(e->url, sizeof e->url, a, "browser_download_url");
        set_local(e);
        snprintf(e->what, sizeof e->what, "One firmware, many synths: CS-1, Scintilla and OberVave as modules in flash, the sine "
                 "built in; HOME opens the picker. Installing a new fm1-dock keeps the deployed modules.");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(a, "size");
        e->size = cJSON_IsNumber(size) ? size->valuedouble : 0;
        const cJSON *digest = cJSON_GetObjectItemCaseSensitive(a, "digest");   /* "sha256:..." */
        if (cJSON_IsString(digest) && strncmp(digest->valuestring, "sha256:", 7) == 0)
            snprintf(e->sha256, sizeof e->sha256, "%s", digest->valuestring + 7);
        break;
    }
    cJSON_Delete(root);
}

bool catalog_fetch(bool *offline, char *err, size_t errlen)
{
    static bool hooked;
    if (!hooked) {
        cJSON_Hooks hooks = { ps_malloc, free };
        cJSON_InitHooks(&hooks);   /* the Hub's catalog is some 5000 nodes: PSRAM, not the internal heap */
        hooked = true;
    }
    sd_mount();
    bool ok = load_hub(offline, err, errlen);
    load_dock();
    mark_on_card();
    qsort(catalog, catalog_n, sizeof *catalog, by_name);
    return ok;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *de; (de = readdir(d));) {
        size_t l = strlen(de->d_name);
        if (l < 6 || strcasecmp(de->d_name + l - 5, ".fwsc") != 0 || de->d_name[0] == '.') continue;
        fw_entry_t *e = add(SRC_SD);
        if (!e) break;
        char path[sizeof e->url];
        if (strlen(dir) + 1 + l >= sizeof path) {
            catalog_n--;
            continue;
        }
        strcpy(path, dir);
        strcat(path, "/");
        strcat(path, de->d_name);
        struct stat st;
        e->size = stat(path, &st) == 0 ? st.st_size : 0;
        snprintf(e->url, sizeof e->url, "%s", path);
        snprintf(e->local, sizeof e->local, "%s", path);
        e->on_card = true;
        snprintf(e->id, sizeof e->id, "%.*s", (int)(l - 5), de->d_name);
        snprintf(e->name, sizeof e->name, "%.*s", (int)(l - 5), de->d_name);
        snprintf(e->author, sizeof e->author, "SD card");
        snprintf(e->what, sizeof e->what, "%s", path);
    }
    closedir(d);
}

int catalog_scan_sd(void)
{
    drop(SRC_SD);
    if (!sd_try_mount()) return 0;
    int before = catalog_n;
    scan_dir(BOARD_SD_MOUNT_POINT);
    scan_dir(CARD_FM1);   /* not its hub/ mirror: those are the catalog's */
    mark_on_card();
    qsort(catalog, catalog_n, sizeof *catalog, by_name);
    return catalog_n - before;
}

/* the Hub's ident patterns: literal characters and [...] classes with ranges */
static bool ident_match(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '[') {
            const char *end = strchr(pat, ']');
            if (!end || !*s) return false;
            bool in = false;
            for (const char *c = pat + 1; c < end; c++) {
                if (c + 2 < end && c[1] == '-') {
                    in |= *s >= c[0] && *s <= c[2];
                    c += 2;
                } else {
                    in |= *s == *c;
                }
            }
            if (!in) return false;
            pat = end + 1;
            s++;
        } else {
            if (*pat++ != *s++) return false;
        }
    }
    return !*s;
}

const fw_entry_t *catalog_installed(const char *identity)
{
    for (int i = 0; i < catalog_n; i++)
        if (catalog[i].src != SRC_SD && strcmp(catalog[i].product, identity) == 0) return &catalog[i];
    for (int i = 0; i < catalog_n; i++)
        if (catalog[i].ident[0] && ident_match(catalog[i].ident, identity)) return &catalog[i];
    return NULL;
}

/* -------------------------------------------------------------------------------- packages --- */

/* the package's bytes: from the card when it is there, else downloaded */
static int fetch(const fw_entry_t *e, uint8_t **raw, void (*progress)(size_t, size_t), bool *from_card, char *why, size_t whylen)
{
    char err[64];
    *from_card = e->on_card && sd_mount();
    if (*from_card) {
        int n = read_file(e->local, raw, PKG_MAX);
        if (n >= 0) return n;
        if (e->src == SRC_SD) {
            snprintf(why, whylen, "cannot read %s", e->local);
            return -1;
        }
        *from_card = false;   /* the mirror's copy is unreadable: download it again */
    }
    int n = net_get(e->url, raw, PKG_MAX, progress, err, sizeof err);
    if (n < 0) snprintf(why, whylen, "download failed: %s", err);
    return n;
}

static void sha256_hex(const uint8_t *buf, size_t n, char out[65])
{
    uint8_t d[32];
    mbedtls_sha256(buf, n, d, 0);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* the size and digest the catalog gives; NULL when they hold */
static const char *as_listed(const fw_entry_t *e, size_t n, const char *sha)
{
    if (e->size && n != e->size) return "its size is not the catalog's: not the package it lists";
    if (e->sha256[0] && strcasecmp(sha, e->sha256) != 0) return "its SHA-256 is not the catalog's: not the package it lists";
    return NULL;
}

bool package_prepare(const fw_entry_t *e, fw_package_t *p, void (*progress)(size_t, size_t), char *why, size_t whylen)
{
    uint8_t *raw;
    bool from_card;
    memset(p, 0, sizeof *p);
    p->entry = *e;
    int n = fetch(e, &raw, progress, &from_card, why, whylen);
    if (n < 0) return false;
    p->from_card = from_card;
    sha256_hex(raw, n, p->sha256);
    const char *listed = as_listed(e, n, p->sha256);
    if (listed) {
        snprintf(why, whylen, "%s%s", listed, from_card ? " (the card's copy: delete it, or save it again)" : "");
        goto bad;
    }
    if (!fm1_product_of(raw, n, p->product)) {
        snprintf(why, whylen, "not an FM-1 package (no identity)");
        goto bad;
    }
    if (e->product[0] && strcmp(p->product, e->product) != 0) {
        snprintf(why, whylen, "the package is %s, the catalog says %s", p->product, e->product);
        goto bad;
    }
    p->official = strcmp(p->sha256, STOCK_V15_SHA256) == 0;
    p->felucca_loader = fm1_has_felucca_loader(raw, n);
    /* Felucca's update loader is what makes an interrupted install resumable; fm1-dock's and Felucca's
     * installers take only those packages, and the official V15 */
    if (!p->felucca_loader && !p->official) {
        snprintf(why, whylen, "no Felucca update loader in this package: only Felucca-based packages and the official "
                 "V15 are installed here");
        goto bad;
    }
    p->n = fm1_logical_image(raw, n, raw);
    p->img = raw;
    char dmg[48];
    const char *damage = p->official ? NULL : fm1_package_damage(p->img, p->n, dmg, sizeof dmg);
    if (damage) {
        snprintf(why, whylen, "damaged package (%s): nothing will be written", damage);
        goto bad;
    }
    p->board = fm1_package_board(p->img, p->n);
    if (p->board == FM1_BOARD_SMK) {
        snprintf(why, whylen, "this package is for the SMK-37, not the FM-1");
        goto bad;
    }
    return true;
bad:
    free(raw);
    p->img = NULL;
    return false;
}

void package_free(fw_package_t *p)
{
    free(p->img);
    p->img = NULL;
    p->n = 0;
}

static void set_on_card(const char *local, bool on)
{
    for (int i = 0; i < catalog_n; i++)
        if (catalog[i].src != SRC_SD && strcmp(catalog[i].local, local) == 0) catalog[i].on_card = on;
}

bool catalog_save(const fw_entry_t *e, void (*progress)(size_t, size_t), char *why, size_t whylen)
{
    char sha[65];
    uint8_t *raw;
    bool from_card;
    if (e->src == SRC_SD || !e->local[0]) {
        snprintf(why, whylen, "it has no place in the card's mirror");
        return false;
    }
    if (!sd_mount()) {
        snprintf(why, whylen, "no SD card");
        return false;
    }
    fw_entry_t net = *e;
    net.on_card = false;   /* always from the net: the card's copy is what is being replaced */
    int n = fetch(&net, &raw, progress, &from_card, why, whylen);
    if (n < 0) return false;
    sha256_hex(raw, n, sha);
    const char *listed = as_listed(e, n, sha);
    bool ok = !listed && write_file(e->local, raw, n);
    free(raw);
    if (listed) snprintf(why, whylen, "%s", listed);
    else if (!ok) snprintf(why, whylen, "writing to the card failed");
    if (ok) set_on_card(e->local, true);
    return ok;
}

bool catalog_remove(const fw_entry_t *e)
{
    if (e->src == SRC_SD || !e->local[0] || !sd_mount() || unlink(e->local) != 0) return false;
    set_on_card(e->local, false);
    return true;
}

int catalog_save_all(void (*progress)(int done, int total, const char *name), volatile bool *stop, int *failed)
{
    char why[96];
    int total = 0, done = 0, saved = 0;
    *failed = 0;
    if (!sd_mount()) return -1;
    for (int i = 0; i < catalog_n; i++) total += catalog[i].src != SRC_SD && !catalog[i].on_card && catalog[i].local[0];
    for (int i = 0; i < catalog_n && !*stop; i++) {
        fw_entry_t *e = &catalog[i];
        if (e->src == SRC_SD || e->on_card || !e->local[0]) continue;
        if (progress) progress(done, total, e->name);
        if (catalog_save(e, NULL, why, sizeof why)) {
            saved++;
        } else {
            (*failed)++;
            ESP_LOGW(TAG, "%s: not saved: %s", e->name, why);
        }
        done++;
    }
    if (progress) progress(done, total, "");
    return saved;
}

/* ---------------------------------------------------------------------- fm1-dock's modules --- */
/* fm1-dock's page lists them (modules/modules.json: id -> file, version, title, by, source); the card
 * keeps the list and the modules saved (/sdcard/fm1/modules), and its other .dock files are listed too */

#define DOCK_PAGE "https://joakimeriksson.github.io/fm1-dock/modules/"
#define MOD_MIRROR CARD_FM1 "/modules"
#define MOD_CACHE MOD_MIRROR "/modules.json"
#define MOD_MAX (512 * 1024)

module_entry_t *modules;
int modules_n;

static module_entry_t *mod_add(void)
{
    if (!modules) modules = heap_caps_calloc(MODULES_MAX, sizeof *modules, MALLOC_CAP_SPIRAM);
    if (!modules || modules_n >= MODULES_MAX) return NULL;
    module_entry_t *m = &modules[modules_n++];
    memset(m, 0, sizeof *m);
    return m;
}

static void mod_scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *de; (de = readdir(d));) {
        size_t l = strlen(de->d_name);
        if (l < 6 || strcasecmp(de->d_name + l - 5, ".dock") != 0 || de->d_name[0] == '.') continue;
        if (strlen(dir) + 1 + l >= sizeof modules->local) continue;
        bool listed = false;   /* the mirror's copies are the page's entries */
        for (int i = 0; i < modules_n && !listed; i++) {
            const char *b = strrchr(modules[i].local, '/');
            listed = modules[i].from_page && b && strcmp(dir, MOD_MIRROR) == 0 && strcmp(b + 1, de->d_name) == 0;
        }
        if (listed) continue;
        module_entry_t *m = mod_add();
        if (!m) break;
        strcpy(m->local, dir);
        strcat(m->local, "/");
        strcat(m->local, de->d_name);
        snprintf(m->title, sizeof m->title, "%.*s", (int)(l - 5), de->d_name);
        snprintf(m->by, sizeof m->by, "SD card");
        snprintf(m->source, sizeof m->source, "%s", m->local);
        struct stat st;
        m->size = stat(m->local, &st) == 0 ? st.st_size : 0;
        m->on_card = true;
    }
    closedir(d);
}

bool modules_fetch(bool *offline, char *err, size_t errlen)
{
    modules_n = 0;
    cJSON *root = get_json(DOCK_PAGE "modules.json", MOD_CACHE, 64 * 1024, offline, err, errlen);
    const cJSON *it;
    if (root) {
        cJSON_ArrayForEach(it, root) {
            const cJSON *file = cJSON_GetObjectItemCaseSensitive(it, "file");
            if (!cJSON_IsString(file) || strchr(file->valuestring, '/')) continue;
            module_entry_t *m = mod_add();
            if (!m) break;
            m->from_page = true;
            snprintf(m->id, sizeof m->id, "%s", it->string ? it->string : "");
            copy(m->title, sizeof m->title, it, "title");
            copy(m->version, sizeof m->version, it, "version");
            copy(m->by, sizeof m->by, it, "by");
            if (!m->by[0]) snprintf(m->by, sizeof m->by, "joakimeriksson");
            copy(m->source, sizeof m->source, it, "source");
            snprintf(m->url, sizeof m->url, DOCK_PAGE "%s", file->valuestring);
            snprintf(m->local, sizeof m->local, MOD_MIRROR "/%s", file->valuestring);
            struct stat st;
            m->on_card = sd_mounted && stat(m->local, &st) == 0;
        }
        cJSON_Delete(root);
    }
    if (sd_mounted) {
        mod_scan_dir(MOD_MIRROR);
        mod_scan_dir(CARD_FM1);
        mod_scan_dir(BOARD_SD_MOUNT_POINT);
    }
    return root != NULL;
}

int module_fetch(const module_entry_t *m, uint8_t **blob, void (*progress)(size_t, size_t), char *why, size_t whylen)
{
    char err[64];
    if (m->on_card && sd_mounted) {
        int n = read_file(m->local, blob, MOD_MAX);
        if (n >= 0 || !m->from_page) {
            if (n < 0) snprintf(why, whylen, "cannot read %s", m->local);
            return n;
        }
    }
    int n = net_get(m->url, blob, MOD_MAX, progress, err, sizeof err);
    if (n < 0) snprintf(why, whylen, "download failed: %s", err);
    return n;
}

static void mod_set_on_card(const char *local, bool on)
{
    for (int i = 0; i < modules_n; i++)
        if (modules[i].from_page && strcmp(modules[i].local, local) == 0) modules[i].on_card = on;
}

bool module_save(const module_entry_t *m, void (*progress)(size_t, size_t), char *why, size_t whylen)
{
    char err[64];
    uint8_t *blob;
    if (!m->from_page) {
        snprintf(why, whylen, "it is the card's already");
        return false;
    }
    if (!sd_mounted) {
        snprintf(why, whylen, "no SD card");
        return false;
    }
    int n = net_get(m->url, &blob, MOD_MAX, progress, err, sizeof err);
    if (n < 0) {
        snprintf(why, whylen, "download failed: %s", err);
        return false;
    }
    bool module = n >= 4 && memcmp(blob, "HRBR", 4) == 0;
    bool ok = module && write_file(m->local, blob, n);
    free(blob);
    if (!ok) snprintf(why, whylen, "%s", module ? "writing to the card failed" : "not a module");
    if (ok) mod_set_on_card(m->local, true);
    return ok;
}

bool module_remove(const module_entry_t *m)
{
    if (!m->from_page || !sd_mounted || unlink(m->local) != 0) return false;
    mod_set_on_card(m->local, false);
    return true;
}
