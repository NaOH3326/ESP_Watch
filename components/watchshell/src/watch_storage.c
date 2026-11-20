#include "watch_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "WATCH_FS";
static bool s_ready;
static wl_handle_t s_wl;
static char s_error[96];

static const char *s_default_scripts[] = {
    "# name: 00_welcome\n"
    "# desc: WatchShell boot demo\n"
    "clear\n"
    "echo green \"WATCHSHELL / ESP32-S3\"\n"
    "echo dim \"----------------------------------------\"\n"
    "echo \"RAM  \"\n"
    "free\n"
    "echo \"SYS  \"\n"
    "info\n"
    "echo cyan \"Use UP/DOWN to choose a script, EXEC to run.\"\n"
    "echo \"\"\n",

    "# name: 05_loop\n"
    "# desc: variable / goto / colored echo\n"
    "clear\n"
    "set N 5\n"
    "label loop\n"
    "echo cyan \"countdown ${N}\"\n"
    "set N ${N}-1\n"
    "if N > 0 goto loop\n"
    "echo green \"launch.\"\n",

    "# name: 10_sensors\n"
    "# desc: IMU / compass / AHT20 snapshot\n"
    "clear\n"
    "echo amber \"SENSOR SNAPSHOT\"\n"
    "sensors\n"
    "echo dim \"done.\"\n",

    "# name: 20_scan\n"
    "# desc: Wi-Fi scan\n"
    "clear\n"
    "echo green \"WIFI SCAN\"\n"
    "wifi scan\n",

    "# name: 30_tone\n"
    "# desc: speaker test\n"
    "clear\n"
    "echo cyan \"1 kHz / 400 ms\"\n"
    "tone 1000 400\n"
    "echo green \"done.\"\n",

    "# name: 40_matrix\n"
    "# desc: terminal decoration\n"
    "clear\n"
    "matrix 120\n",

    "# name: 45_cmatrix\n"
    "# desc: cmatrix-style scrolling rain\n"
    "clear\n"
    "prompt \"Wake up, Neo...\"\n"
    "echo green \"WAKE UP, NEO...\"\n"
    "echo dim \"the watch has you\"\n"
    "sleep 900\n"
    "clear\n"
    "prompt \"matrix>\"\n"
    "label rain\n"
    "matrix\n"
    "sleep 60\n"
    "goto rain\n",

    "# name: 90_reboot\n"
    "# desc: reboot the watch\n"
    "echo red \"rebooting...\"\n"
    "sleep 300\n"
    "reboot\n",
};

static void mkdir_if_needed(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

static void install_defaults(void)
{
    static const char *names[] = {
        "00_welcome.wsh", "05_loop.wsh", "10_sensors.wsh", "20_scan.wsh",
        "30_tone.wsh", "40_matrix.wsh", "45_cmatrix.wsh", "90_reboot.wsh",
    };
    char path[96];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), WS_SCRIPT_DIR "/%s", names[i]);
        struct stat st;
        if (stat(path, &st) == 0) {
            continue;   /* never overwrite a user-edited script */
        }
        watch_storage_write_all(path, s_default_scripts[i], strlen(s_default_scripts[i]));
    }
    ESP_LOGI(TAG, "default scripts checked");
}

esp_err_t watch_storage_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(WS_STORAGE_ROOT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "mount: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "FAT mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    mkdir_if_needed(WS_SCRIPT_DIR);
    mkdir_if_needed(WS_MUSIC_DIR);
    mkdir_if_needed(WS_VIDEO_DIR);
    install_defaults();
    ESP_LOGI(TAG, "FAT mounted at %s", WS_STORAGE_ROOT);
    return ESP_OK;
}

bool watch_storage_ready(void)
{
    return s_ready;
}

static bool match_one_ext(const char *name, size_t name_len,
                          const char *ext, size_t ext_len)
{
    if (ext_len == 0) {
        return true;
    }
    if (name_len < ext_len) {
        return false;
    }
    for (size_t i = 0; i < ext_len; i++) {
        char a = name[name_len - ext_len + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* ext may be a comma-separated list, e.g. ".jpg,.jpeg". */
static bool match_ext(const char *name, const char *ext)
{
    if (!ext || !ext[0]) {
        return true;
    }
    size_t name_len = strlen(name);
    while (*ext) {
        const char *comma = strchr(ext, ',');
        size_t ext_len = comma ? (size_t)(comma - ext) : strlen(ext);
        if (match_one_ext(name, name_len, ext, ext_len)) {
            return true;
        }
        if (!comma) {
            break;
        }
        ext = comma + 1;
    }
    return false;
}

int watch_storage_list(const char *dir, const char *ext,
                       char names[][48], int max_names)
{
    if (!s_ready || !dir || !names || max_names <= 0) {
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max_names) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (!match_ext(de->d_name, ext)) {
            continue;
        }
        strncpy(names[n], de->d_name, 47);
        names[n][47] = '\0';
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++) {
        char tmp[48];
        memcpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcasecmp(names[j], tmp) > 0) {
            memcpy(names[j + 1], names[j], 48);
            j--;
        }
        memcpy(names[j + 1], tmp, 48);
    }
    return n;
}

int watch_storage_list_scripts(char names[][48], int max_names)
{
    return watch_storage_list(WS_SCRIPT_DIR, ".wsh", names, max_names);
}

int watch_storage_list_music(char names[][48], int max_names)
{
    return watch_storage_list(WS_MUSIC_DIR, NULL, names, max_names);
}

esp_err_t watch_storage_read_all_caps(const char *path, char **out, size_t *out_len,
                                      size_t max_len, bool prefer_psram)
{
    if (!path || !out || !out_len || !max_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(s_error, sizeof(s_error), "open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > max_len) {
        fclose(f);
        snprintf(s_error, sizeof(s_error), "size %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);
    char *buf = NULL;
    if (prefer_psram) {
        buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        buf = malloc((size_t)sz + 1);
    }
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

esp_err_t watch_storage_read_all(const char *path, char **out, size_t *out_len)
{
    return watch_storage_read_all_caps(path, out, out_len, 128 * 1024, false);
}

esp_err_t watch_storage_write_all(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(s_error, sizeof(s_error), "create %s", path);
        return ESP_FAIL;
    }
    size_t wrote = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    return wrote == len ? ESP_OK : ESP_FAIL;
}

esp_err_t watch_storage_delete(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t root_len = strlen(WS_STORAGE_ROOT);
    if (strncmp(path, WS_STORAGE_ROOT, root_len) != 0 ||
        (path[root_len] != '/' && path[root_len] != '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) != 0) {
        snprintf(s_error, sizeof(s_error), "delete %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char *watch_storage_error(void)
{
    return s_error;
}
