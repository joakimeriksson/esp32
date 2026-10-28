/* SPDX-License-Identifier: GPL-3.0-only */
/* The console on UART0 (idf.py monitor, or any terminal at the console baud): what the screen does
 * on fm1-flasher-p4. `help` lists the commands; the lists are numbered by `ls`, the numbers are
 * what deploy / get / save take. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "flasher.h"
#include "net.h"

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        char ssid[33];
        net_ssid(ssid, sizeof ssid);
        printf("WiFi: %s%s\n", ssid[0] ? ssid : "no network saved", net_state() == NET_UP ? ", up" : "");
        return 0;
    }
    flasher_wifi_connect(argv[1], argc > 2 ? argv[2] : "");
    return 0;
}

static int cmd_scan(int argc, char **argv) { flasher_wifi_scan(); return 0; }
static int cmd_refresh(int argc, char **argv) { flasher_refresh(); return 0; }
static int cmd_browse(int argc, char **argv) { flasher_browse(); return 0; }

static int cmd_dev(int argc, char **argv)
{
    dev_info_t d;
    flasher_device(&d);
    static const char *const S[] = { "nothing on the port", "asking", "an FM-1", "in update mode", "another MIDI device" };
    printf("USB: %s %s %s\n", S[d.state], d.usb_name, d.id.text);
    if (d.dock) {
        fm1_dock_module_t m[FM1_DOCK_MODS_MAX];
        int n = flasher_dock_modules(m, FM1_DOCK_MODS_MAX);
        printf("fm1-dock ABI %u, %lu KB free, room for %lu KB; %d modules:\n", d.hello.abi, (unsigned long)(d.hello.free / 1024),
               (unsigned long)(d.hello.largest / 1024), n);
        for (int i = 0; i < n; i++)
            printf("  %s%-23s v%-4lu %-10s %lu KB\n", i == d.hello.cur ? "*" : " ", m[i].name, (unsigned long)m[i].version,
                   fm1_dock_state(m[i].state), (unsigned long)(m[i].size / 1024));
    }
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    static const char *const SRC[] = { "hub", "official", "fm1-dock", "card" };
    printf("modules (fm1-dock's page and the card): m<n>\n");
    for (int i = 0; i < modules_n; i++)
        printf("  m%-3d %-22s %-8s %5lu KB %s%s\n", i, modules[i].title, modules[i].version, (unsigned long)((modules[i].size + 1023) / 1024),
               modules[i].on_card ? "card " : "", modules[i].from_page ? "" : "(card only)");
    printf("firmwares: f<n>\n");
    for (int i = 0; i < catalog_n; i++)
        printf("  f%-3d %-24s %-10s %-8s %5lu KB %s%s%s\n", i, catalog[i].name, catalog[i].version, SRC[catalog[i].src],
               (unsigned long)((catalog[i].size + 1023) / 1024), catalog[i].on_card ? "card " : "", catalog[i].beta ? "beta " : "",
               catalog[i].product);
    return 0;
}

/* "m3" / "f12": the entry, or NULL */
static const void *entry(const char *s, bool *module)
{
    if (!s || (s[0] != 'm' && s[0] != 'f')) return NULL;
    char *end;
    long i = strtol(s + 1, &end, 10);
    if (*end || i < 0) return NULL;
    *module = s[0] == 'm';
    if (*module) return i < modules_n ? &modules[i] : NULL;
    return i < catalog_n ? &catalog[i] : NULL;
}

static int cmd_get(int argc, char **argv)
{
    bool module;
    const void *e = argc > 1 ? entry(argv[1], &module) : NULL;
    if (!e) {
        printf("get f<n>: fetch and check a firmware (then: install yes); get m<n>: deploy a module\n");
        return 1;
    }
    if (module) flasher_mod_deploy(e);
    else flasher_prepare(e);
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("install yes: the firmware fetched with get, on the FM-1, at your own risk (README)\n");
        return 1;
    }
    flasher_install();
    return 0;
}

static int cmd_save(int argc, char **argv)
{
    bool module;
    if (argc > 1 && strcmp(argv[1], "all") == 0) {
        flasher_save_all();
        return 0;
    }
    const void *e = argc > 1 ? entry(argv[1], &module) : NULL;
    if (!e) {
        printf("save f<n> | m<n> | all: onto the card's mirror; rm f<n> | m<n>: off it\n");
        return 1;
    }
    if (module) flasher_mod_save(e);
    else flasher_save(e);
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    bool module;
    const void *e = argc > 1 ? entry(argv[1], &module) : NULL;
    if (!e) return 1;
    if (module) flasher_mod_remove(e);
    else flasher_remove(e);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    if (argc < 2) return 1;
    flasher_dock_select(argv[1]);
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    if (argc < 2) return 1;
    flasher_dock_delete(argv[1]);
    return 0;
}

static int cmd_library(int argc, char **argv)
{
    if (argc < 2) return 1;
    flasher_library(strcmp(argv[1], "on") == 0);
    return 0;
}

static void reg(const char *name, const char *help, const char *hint, esp_console_cmd_func_t f)
{
    const esp_console_cmd_t c = { .command = name, .help = help, .hint = hint, .func = f };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void)
{
    esp_console_repl_t *repl;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    rc.prompt = "dock>";
    rc.max_cmdline_length = 160;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));
    esp_console_register_help_command();
    reg("wifi", "the network: wifi SSID [password] connects and saves it; alone: which", "[ssid] [password]", cmd_wifi);
    reg("scan", "the networks around", NULL, cmd_scan);
    reg("dev", "what is on the USB port (an FM-1, its fm1-dock modules)", NULL, cmd_dev);
    reg("ls", "the library: modules m<n>, firmwares f<n>", NULL, cmd_ls);
    reg("refresh", "the catalog, fm1-dock's page and the card again", NULL, cmd_refresh);
    reg("get", "get f<n>: fetch and check a firmware; get m<n>: deploy a module onto the FM-1", "f<n> | m<n>", cmd_get);
    reg("install", "install yes: the firmware fetched with get, onto the FM-1, at your own risk", "yes", cmd_install);
    reg("save", "onto the card's mirror: save f<n> | m<n> | all (all again: stop)", "f<n> | m<n> | all", cmd_save);
    reg("rm", "off the card's mirror: rm f<n> | m<n>", "f<n> | m<n>", cmd_rm);
    reg("play", "play NAME: the FM-1's module playing", "NAME", cmd_play);
    reg("delete", "delete NAME: the FM-1's module deleted", "NAME", cmd_delete);
    reg("browse", "the browser onto the FM-1 now (it is sent by itself once per plug-in)", NULL, cmd_browse);
    reg("library", "library on | off: the browser sent when an fm1-dock is plugged in", "on | off", cmd_library);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
