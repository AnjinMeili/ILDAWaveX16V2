#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "app_config.h"
#include "web_server.h"
#include "net_manager.h"
#include "rp_manager.h"
#include "ilda_player.h"
#include "sd_manager.h"
#include "protocol_stream_stats.h"
#include "protocol_settings.h"
#include "idn_protocol.h"
#include "etherdream_protocol.h"
#include "ilw_protocol.h"

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;
static TaskHandle_t s_refresh_task = NULL;

#define WEB_SERVER_TASK_STACK_BYTES 16384
#define WEB_SD_LIST_JSON_BYTES 2048
#define WEB_RP_STATUS_JSON_BYTES 1920
#define WEB_NETWORK_JSON_BYTES 2048
#define WEB_PROTOCOL_JSON_BYTES 960

/* ===================== BACKGROUND DAC ENGINE REFRESH ===================== */

static void dac_engine_refresh_task(void *arg)
{
    (void)arg;

    while (1) {
        rp_manager_ping();
        rp_manager_get_remote_status();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================== URL DECODE ===================== */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);

            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }

        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

/* ===================== HELPERS ===================== */

static void set_common_response_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t send_plain_result(httpd_req_t *req, esp_err_t ret)
{
    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");

    if (ret == ESP_OK) {
        return httpd_resp_sendstr(req, "OK");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "ERR %s", esp_err_to_name(ret));
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t send_bad_request(httpd_req_t *req, const char *msg)
{
    set_common_response_headers(req);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");

    char buf[160];
    snprintf(buf, sizeof(buf), "ERR %s", msg ? msg : "Bad request");
    return httpd_resp_sendstr(req, buf);
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t off = 0;

    if (!src) {
        src = "";
    }

    for (size_t i = 0; src[i] && off + 1 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];

        if ((ch == '"' || ch == '\\') && off + 2 < dst_len) {
            dst[off++] = '\\';
            dst[off++] = (char)ch;
        } else if (ch >= 0x20) {
            dst[off++] = (char)ch;
        }
    }

    dst[off] = '\0';
}

static void copy_bounded_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    size_t len = strnlen(src, dst_len - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    if (!body || !key || !out || out_len == 0) {
        return false;
    }

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);

        if (eq) {
            size_t this_key_len = (size_t)(eq - p);

            if (this_key_len == key_len && strncmp(p, key, key_len) == 0) {
                size_t value_len = pair_len - this_key_len - 1;
                if (value_len >= out_len) {
                    value_len = out_len - 1;
                }

                memcpy(out, eq + 1, value_len);
                out[value_len] = '\0';
                url_decode_inplace(out);
                return true;
            }
        }

        if (!amp) {
            break;
        }

        p = amp + 1;
    }

    return false;
}

static bool form_value_enabled(const char *value)
{
    return strcmp(value, "1") == 0 ||
           strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "enabled") == 0 ||
           strcasecmp(value, "on") == 0;
}

static esp_err_t read_request_body(httpd_req_t *req, char *body, size_t body_len)
{
    int total = req->content_len;

    if (total < 0 || (size_t)total >= body_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);

        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (ret <= 0) {
            return ESP_FAIL;
        }

        received += ret;
    }

    body[received] = '\0';
    return ESP_OK;
}

/* ===================== SD LIST ===================== */

static esp_err_t sd_list_get_handler(httpd_req_t *req)
{
    (void)sd_manager_rescan();

    sd_status_t sd = {0};
    sd_manager_get_status(&sd);

    char *json = malloc(WEB_SD_LIST_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    size_t off = 0;
    int count = 0;

    if (!sd.mounted) {
        free(json);
        set_common_response_headers(req);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"mounted\":false,\"count\":0,\"files\":[]}");
    }

    DIR *dir = opendir("/sdcard");

    if (!dir) {
        free(json);
        set_common_response_headers(req);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"mounted\":false,\"count\":0,\"files\":[]}");
    }

    off += snprintf(json + off, WEB_SD_LIST_JSON_BYTES - off,
                    "{\"mounted\":true,\"files\":[");

    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        char path[320];
        snprintf(path, sizeof(path), "/sdcard/%s", ent->d_name);

        struct stat st;
        memset(&st, 0, sizeof(st));

        if (stat(path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            continue;
        }

        const char *type = "file";
        const char *dot = strrchr(ent->d_name, '.');

        if (dot) {
            if (strcasecmp(dot, ".ild") == 0) {
                type = "ILDA";
            } else if (strcasecmp(dot, ".txt") == 0) {
                type = "Text";
            } else {
                type = dot + 1;
            }
        }

        if (count > 0) {
            off += snprintf(json + off, WEB_SD_LIST_JSON_BYTES - off, ",");
        }

        off += snprintf(json + off, WEB_SD_LIST_JSON_BYTES - off,
                        "{\"name\":\"%s\",\"path\":\"%s\",\"size\":%lu,\"type\":\"%s\",\"is_dir\":false}",
                        ent->d_name,
                        path,
                        (unsigned long)st.st_size,
                        type);

        count++;

        if (count >= 48) {
            break;
        }

        if (off > WEB_SD_LIST_JSON_BYTES - 256) {
            break;
        }
    }

    closedir(dir);

    off += snprintf(json + off, WEB_SD_LIST_JSON_BYTES - off, "],\"count\":%d}", count);

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

/* ===================== STATUS ===================== */

static esp_err_t rp_status_get_handler(httpd_req_t *req)
{
    rp_status_t rp;
    rp_manager_get_status(&rp);

    sd_status_t sd = {0};
    sd_manager_get_status(&sd);

    protocol_stream_stats_t streams = {0};
    protocol_stream_stats_get(&streams);

    char *json = malloc(WEB_RP_STATUS_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    snprintf(json, WEB_RP_STATUS_JSON_BYTES,
        "{"
            "\"product\":\"%s\","
            "\"esp_fw\":\"%s\","
            "\"rp_fw\":\"%s\","
            "\"initialized\":%s,"
            "\"remote_running\":%s,"
            "\"remote_sample_rate\":%lu,"
            "\"remote_payload_errors\":%lu,"
            "\"remote_buffer_fill_samples\":%lu,"
            "\"remote_buffer_size_samples\":%lu,"
            "\"remote_buffer_high_water_samples\":%lu,"
            "\"remote_buffer_low_water_samples\":%lu,"
            "\"remote_buffer_max_samples\":%lu,"
            "\"remote_mirror_x\":%s,"
            "\"remote_mirror_y\":%s,"
            "\"remote_underruns\":%lu,"
            "\"remote_stream_packets\":%lu,"
            "\"remote_stream_bad_packets\":%lu,"
            "\"remote_stream_dropped_packets\":%lu,"
            "\"remote_stream_rx_timeouts\":%lu,"
            "\"sd_inserted\":%s,"
            "\"sd_mounted\":%s,"
            "\"sd_file_count\":%d,"
            "\"stream_sd_ilda_samples\":%lu,"
            "\"stream_idn_samples\":%lu,"
            "\"stream_etherdream_samples\":%lu,"
            "\"stream_ilw_samples\":%lu,"
            "\"test_stream_running\":%s,"
            "\"ilda_running\":%s"
        "}",
        PRODUCT_NAME,
        FW_VERSION,
        rp.remote_fw_version[0] ? rp.remote_fw_version : "unknown",
        rp.initialized ? "true" : "false",
        rp.remote_running ? "true" : "false",
        (unsigned long)rp.remote_sample_rate,
        (unsigned long)rp.remote_payload_errors,
        (unsigned long)rp.remote_buffer_fill_samples,
        (unsigned long)rp.remote_buffer_size_samples,
        (unsigned long)rp.remote_buffer_high_water_samples,
        (unsigned long)rp.remote_buffer_low_water_samples,
        (unsigned long)rp.remote_buffer_max_samples,
        rp.remote_mirror_x ? "true" : "false",
        rp.remote_mirror_y ? "true" : "false",
        (unsigned long)rp.remote_underruns,
        (unsigned long)rp.remote_stream_packets,
        (unsigned long)rp.remote_stream_bad_packets,
        (unsigned long)rp.remote_stream_dropped_packets,
        (unsigned long)rp.remote_stream_rx_timeouts,
        sd.inserted ? "true" : "false",
        sd.mounted ? "true" : "false",
        sd.file_count,
        (unsigned long)streams.sd_ilda_samples,
        (unsigned long)streams.idn_samples,
        (unsigned long)streams.etherdream_samples,
        (unsigned long)streams.ilw_samples,
        rp.test_stream_running ? "true" : "false",
        ilda_player_is_running() ? "true" : "false"
    );

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

/* ===================== NETWORK SETTINGS ===================== */

static esp_err_t network_config_get_handler(httpd_req_t *req)
{
    net_settings_t cfg;
    net_status_t status;

    net_manager_get_settings(&cfg);
    net_manager_get_status(&status);

    char ap_ssid[96];
    char sta_ssid[96];
    char hostname[96];
    char ip[32];
    char mask[32];
    char gw[32];
    char dns[32];

    json_escape(cfg.wifi_ap_ssid, ap_ssid, sizeof(ap_ssid));
    json_escape(cfg.wifi_sta_ssid, sta_ssid, sizeof(sta_ssid));
    json_escape(cfg.hostname, hostname, sizeof(hostname));
    json_escape(cfg.static_ip, ip, sizeof(ip));
    json_escape(cfg.static_netmask, mask, sizeof(mask));
    json_escape(cfg.static_gateway, gw, sizeof(gw));
    json_escape(cfg.static_dns, dns, sizeof(dns));

    char *json = malloc(WEB_NETWORK_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    snprintf(json, WEB_NETWORK_JSON_BYTES,
        "{"
            "\"wifi_ap_enabled\":%s,"
            "\"wifi_ap_ssid\":\"%s\","
            "\"wifi_ap_password_set\":%s,"
            "\"wifi_sta_enabled\":%s,"
            "\"wifi_sta_ssid\":\"%s\","
            "\"wifi_sta_password_set\":%s,"
            "\"hostname\":\"%s\","
            "\"ip_mode\":\"%s\","
            "\"static_ip\":\"%s\","
            "\"static_netmask\":\"%s\","
            "\"static_gateway\":\"%s\","
            "\"static_dns\":\"%s\","
            "\"reboot_required\":%s,"
            "\"status\":{"
                "\"eth_link\":%s,"
                "\"eth_has_ip\":%s,"
                "\"eth_ip\":\"%s\","
                "\"wifi_ap_enabled\":%s,"
                "\"wifi_ap_ip\":\"%s\","
                "\"wifi_sta_enabled\":%s,"
                "\"wifi_sta_connected\":%s,"
                "\"wifi_sta_ip\":\"%s\""
            "}"
        "}",
        cfg.wifi_ap_enabled ? "true" : "false",
        ap_ssid,
        cfg.wifi_ap_password[0] ? "true" : "false",
        cfg.wifi_sta_enabled ? "true" : "false",
        sta_ssid,
        cfg.wifi_sta_password[0] ? "true" : "false",
        hostname,
        cfg.ip_mode == NET_IP_MODE_STATIC ? "static" : "dhcp",
        ip,
        mask,
        gw,
        dns,
        net_manager_settings_pending_reboot() ? "true" : "false",
        status.eth_link ? "true" : "false",
        status.eth_has_ip ? "true" : "false",
        status.eth_ip,
        status.wifi_ap_enabled ? "true" : "false",
        status.wifi_ap_ip,
        status.wifi_sta_enabled ? "true" : "false",
        status.wifi_sta_connected ? "true" : "false",
        status.wifi_sta_ip
    );

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

static esp_err_t network_config_post_handler(httpd_req_t *req)
{
    char body[1024];
    esp_err_t ret = read_request_body(req, body, sizeof(body));

    if (ret != ESP_OK) {
        return send_bad_request(req, "Network settings form is too large");
    }

    net_settings_t cfg;
    net_manager_get_settings(&cfg);

    char value[128];

    if (form_get_value(body, "ap_enabled", value, sizeof(value))) {
        cfg.wifi_ap_enabled = form_value_enabled(value);
    }

    if (form_get_value(body, "ap_ssid", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_ap_ssid, sizeof(cfg.wifi_ap_ssid), value);
    }

    if (form_get_value(body, "ap_password", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_ap_password, sizeof(cfg.wifi_ap_password), value);
    }

    if (form_get_value(body, "sta_enabled", value, sizeof(value))) {
        cfg.wifi_sta_enabled = form_value_enabled(value);
    }

    if (form_get_value(body, "sta_ssid", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_sta_ssid, sizeof(cfg.wifi_sta_ssid), value);
    }

    if (form_get_value(body, "sta_password", value, sizeof(value))) {
        copy_bounded_string(cfg.wifi_sta_password, sizeof(cfg.wifi_sta_password), value);
    }

    if (form_get_value(body, "hostname", value, sizeof(value))) {
        copy_bounded_string(cfg.hostname, sizeof(cfg.hostname), value);
    }

    if (form_get_value(body, "ip_mode", value, sizeof(value))) {
        cfg.ip_mode = strcmp(value, "static") == 0 ? NET_IP_MODE_STATIC : NET_IP_MODE_DHCP;
    }

    if (form_get_value(body, "static_ip", value, sizeof(value))) {
        copy_bounded_string(cfg.static_ip, sizeof(cfg.static_ip), value);
    }

    if (form_get_value(body, "static_netmask", value, sizeof(value))) {
        copy_bounded_string(cfg.static_netmask, sizeof(cfg.static_netmask), value);
    }

    if (form_get_value(body, "static_gateway", value, sizeof(value))) {
        copy_bounded_string(cfg.static_gateway, sizeof(cfg.static_gateway), value);
    }

    if (form_get_value(body, "static_dns", value, sizeof(value))) {
        copy_bounded_string(cfg.static_dns, sizeof(cfg.static_dns), value);
    }

    char err[160];
    ret = net_manager_validate_settings(&cfg, err, sizeof(err));
    if (ret != ESP_OK) {
        return send_bad_request(req, err);
    }

    ret = net_manager_save_settings(&cfg);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    char json[64];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"reboot_required\":%s}",
             net_manager_settings_pending_reboot() ? "true" : "false");

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* ===================== PROTOCOL SETTINGS ===================== */

static esp_err_t protocol_config_get_handler(httpd_req_t *req)
{
    protocol_settings_t cfg;
    protocol_settings_t active;

    protocol_settings_get(&cfg);
    protocol_settings_get_active(&active);

    char *json = malloc(WEB_PROTOCOL_JSON_BYTES);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    snprintf(json, WEB_PROTOCOL_JSON_BYTES,
        "{"
            "\"sd_ilda_enabled\":true,"
            "\"sd_ilda_locked\":true,"
            "\"sd_ilda_transport\":\"Local SD\","
            "\"sd_ilda_ports\":\"None\","
            "\"idn_enabled\":%s,"
            "\"idn_transport\":\"UDP\","
            "\"idn_udp_port\":%d,"
            "\"etherdream_enabled\":%s,"
            "\"etherdream_transport\":\"TCP + UDP\","
            "\"etherdream_tcp_port\":%d,"
            "\"etherdream_udp_port\":%d,"
            "\"ilw_enabled\":%s,"
            "\"ilw_transport\":\"UDP\","
            "\"ilw_udp_port\":%d,"
            "\"reboot_required\":%s,"
            "\"active\":{"
                "\"sd_ilda_enabled\":true,"
                "\"idn_enabled\":%s,"
                "\"etherdream_enabled\":%s,"
                "\"ilw_enabled\":%s"
            "}"
        "}",
        cfg.idn_enabled ? "true" : "false",
        IDN_PROTOCOL_UDP_PORT,
        cfg.etherdream_enabled ? "true" : "false",
        ETHERDREAM_PROTOCOL_TCP_PORT,
        ETHERDREAM_PROTOCOL_UDP_PORT,
        cfg.ilw_enabled ? "true" : "false",
        ILW_PROTOCOL_UDP_PORT,
        protocol_settings_pending_reboot() ? "true" : "false",
        active.idn_enabled ? "true" : "false",
        active.etherdream_enabled ? "true" : "false",
        active.ilw_enabled ? "true" : "false"
    );

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

static esp_err_t protocol_config_post_handler(httpd_req_t *req)
{
    char body[384];
    esp_err_t ret = read_request_body(req, body, sizeof(body));

    if (ret != ESP_OK) {
        return send_bad_request(req, "Protocol settings form is too large");
    }

    protocol_settings_t cfg;
    protocol_settings_get(&cfg);

    char value[32];

    cfg.sd_ilda_enabled = true;

    if (form_get_value(body, "idn_enabled", value, sizeof(value))) {
        cfg.idn_enabled = form_value_enabled(value);
    } else {
        cfg.idn_enabled = false;
    }

    if (form_get_value(body, "etherdream_enabled", value, sizeof(value))) {
        cfg.etherdream_enabled = form_value_enabled(value);
    } else {
        cfg.etherdream_enabled = false;
    }

    if (form_get_value(body, "ilw_enabled", value, sizeof(value))) {
        cfg.ilw_enabled = form_value_enabled(value);
    } else {
        cfg.ilw_enabled = false;
    }

    ret = protocol_settings_save(&cfg);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    char json[64];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"reboot_required\":%s}",
             protocol_settings_pending_reboot() ? "true" : "false");

    set_common_response_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

static esp_err_t system_reboot_post_handler(httpd_req_t *req)
{
    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t ret = httpd_resp_sendstr(req, "OK restarting");

    BaseType_t ok = xTaskCreate(
        reboot_task,
        "web_reboot",
        2048,
        NULL,
        3,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create reboot task");
    }

    return ret;
}

static esp_err_t system_factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t reset_ret = net_manager_restore_defaults();
    if (reset_ret != ESP_OK) {
        return send_plain_result(req, reset_ret);
    }

    reset_ret = protocol_settings_restore_defaults();
    if (reset_ret != ESP_OK) {
        return send_plain_result(req, reset_ret);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t ret = httpd_resp_sendstr(req, "OK factory defaults restored; restarting");

    BaseType_t ok = xTaskCreate(
        reboot_task,
        "web_factory",
        2048,
        NULL,
        3,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create factory reset reboot task");
    }

    return ret;
}

/* ===================== CONTROL HANDLERS ===================== */

static esp_err_t rp_refresh_post_handler(httpd_req_t *req)
{
    rp_manager_ping();
    return send_plain_result(req, rp_manager_get_remote_status());
}

static esp_err_t rp_ping_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, rp_manager_ping());
}

static esp_err_t rp_start_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, rp_manager_output_start());
}

static esp_err_t stop_everything_safe(void)
{
    esp_err_t ilda_ret = ilda_player_stop_and_wait(1500);
    rp_manager_stop_test_stream();
    ilw_protocol_stop();

    esp_err_t stop_ret = rp_manager_output_force_stop();
    esp_err_t clear_ret = rp_manager_clear_buffer();

    if (stop_ret != ESP_OK) {
        return stop_ret;
    }

    if (clear_ret != ESP_OK) {
        return clear_ret;
    }

    return ilda_ret;
}

static esp_err_t rp_stop_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, stop_everything_safe());
}

static esp_err_t rp_clear_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, stop_everything_safe());
}

static esp_err_t rp_start_stream_post_handler(httpd_req_t *req)
{
    char query[64];
    char shape[24];
    rp_test_shape_t test_shape = RP_TEST_SHAPE_SQUARE;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "shape", shape, sizeof(shape)) == ESP_OK) {
        if (strcasecmp(shape, "circle") == 0) {
            test_shape = RP_TEST_SHAPE_CIRCLE;
        } else if (strcasecmp(shape, "cross") == 0) {
            test_shape = RP_TEST_SHAPE_CROSS;
        } else if (strcasecmp(shape, "heart") == 0) {
            test_shape = RP_TEST_SHAPE_HEART;
        } else {
            test_shape = RP_TEST_SHAPE_SQUARE;
        }
    }

    ilda_player_stop();
    return send_plain_result(req, rp_manager_start_test_shape(test_shape));
}

static esp_err_t rp_test_color_post_handler(httpd_req_t *req)
{
    char query[64];
    char param[16];
    uint8_t mask = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "mask", param, sizeof(param)) == ESP_OK) {
        mask = (uint8_t)(strtoul(param, NULL, 10) & 0x07);
    }

    rp_manager_set_test_color_mask(mask);

    rp_status_t status;
    rp_manager_get_status(&status);

    if (mask != 0 && !status.test_stream_running) {
        return send_plain_result(req, rp_manager_start_test_shape(RP_TEST_SHAPE_BEAM));
    }

    return send_plain_result(req, ESP_OK);
}

static esp_err_t rp_stop_stream_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, rp_manager_stop_test_stream());
}

static esp_err_t rp_set_rate_post_handler(httpd_req_t *req)
{
    char query[64];
    char param[32];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_sendstr(req, "ERR missing query");
    }

    if (httpd_query_key_value(query, "value", param, sizeof(param)) != ESP_OK) {
        return httpd_resp_sendstr(req, "ERR missing value");
    }

    uint32_t rate = (uint32_t)strtoul(param, NULL, 10);

    if (rate < 1) {
        rate = 1;
    }

    if (rate > 200000) {
        rate = 200000;
    }

    return send_plain_result(req, rp_manager_set_rate(rate));
}

static esp_err_t rp_buffer_config_post_handler(httpd_req_t *req)
{
    char body[160];
    char value[32];

    esp_err_t ret = read_request_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return send_bad_request(req, "DAC buffer settings form is too large");
    }

    if (!form_get_value(body, "size", value, sizeof(value))) {
        return send_bad_request(req, "Missing buffer size");
    }

    uint32_t size = (uint32_t)strtoul(value, NULL, 10);

    if (!form_get_value(body, "high_water", value, sizeof(value))) {
        return send_bad_request(req, "Missing high-water limit");
    }

    uint32_t high_water = (uint32_t)strtoul(value, NULL, 10);

    if (!form_get_value(body, "low_water", value, sizeof(value))) {
        return send_bad_request(req, "Missing low-water limit");
    }

    uint32_t low_water = (uint32_t)strtoul(value, NULL, 10);

    if (size < 512) {
        return send_bad_request(req, "Buffer size must be at least 512 samples");
    }

    if (high_water < 256) {
        return send_bad_request(req, "High-water limit must be at least 256 samples");
    }

    if (high_water > size - 256) {
        return send_bad_request(req, "High-water limit must leave at least 256 samples of headroom");
    }

    if (low_water > high_water - 256) {
        return send_bad_request(req, "Low-water limit must be at least 256 samples below high-water");
    }

    rp_status_t rp;
    rp_manager_get_status(&rp);

    if (rp.remote_buffer_max_samples > 0 && size > rp.remote_buffer_max_samples) {
        return send_bad_request(req, "Buffer size is above the RP maximum");
    }

    ilda_player_stop_and_wait(1500);

    ret = rp_manager_stop_test_stream();
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    ret = rp_manager_set_buffer_config(size, high_water, low_water);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t rp_mirror_config_post_handler(httpd_req_t *req)
{
    char body[96];
    char value[16];
    bool mirror_x = false;
    bool mirror_y = false;

    esp_err_t ret = read_request_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return send_bad_request(req, "DAC output mapping form is too large");
    }

    if (form_get_value(body, "mirror_x", value, sizeof(value))) {
        mirror_x = form_value_enabled(value);
    }

    if (form_get_value(body, "mirror_y", value, sizeof(value))) {
        mirror_y = form_value_enabled(value);
    }

    ret = rp_manager_set_mirror(mirror_x, mirror_y);
    if (ret != ESP_OK) {
        return send_plain_result(req, ret);
    }

    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t ilda_start_post_handler(httpd_req_t *req)
{
    char query[256];
    char path[192];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        strlcpy(path, "/sdcard/test.ild", sizeof(path));
    } else if (httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
        strlcpy(path, "/sdcard/test.ild", sizeof(path));
    }

    url_decode_inplace(path);

    rp_manager_stop_test_stream();

    return send_plain_result(req, ilda_player_start(path));
}

static esp_err_t ilda_stop_post_handler(httpd_req_t *req)
{
    return send_plain_result(req, stop_everything_safe());
}

/* ===================== UI ===================== */

static const char *html_page =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ILDAWaveX16</title>"
"<style>"
":root{--bg:#1f2429;--side:#191e23;--top:#20262b;--panel:#20262b;--panel2:#262c32;--line:#3a424a;--text:#eef3f7;--muted:#9aa5ad;--green:#15985c;--green2:#1dbf73;--red:#d94b4b;--amber:#d99b28;--blue:#4aa3ff}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,-apple-system,'Helvetica Neue',Arial,sans-serif;font-size:14px}"
".app{display:flex;min-height:100vh}.side{width:292px;background:var(--side);position:fixed;inset:0 auto 0 0;padding:18px 10px;display:flex;flex-direction:column;border-right:1px solid #15191d}"
".brand{padding:0 10px 20px}.brand h1{margin:0;font-size:24px;line-height:1.08;font-weight:900;letter-spacing:0;background:linear-gradient(90deg,#e35d5b,#f0b44c,#47c97a,#4aa3ff,#a77bff,#e35d5b);background-size:500% 500%;-webkit-background-clip:text;background-clip:text;color:transparent;animation:rainbowShift 9s ease-in-out infinite;white-space:nowrap}.brand p{margin:5px 0 0;color:#dbe3ea;font-size:12px}"
"@keyframes rainbowShift{0%{background-position:0% 50%}50%{background-position:100% 50%}100%{background-position:0% 50%}}"
".nav button{width:100%;height:39px;border:0;border-radius:6px;background:transparent;color:#d8e0e6;text-align:left;padding:0 13px;margin:2px 0;cursor:pointer;font-weight:600;font-family:inherit}.nav button:hover{background:#242b31}.nav button.active{background:var(--green);color:white}"
".main{margin-left:292px;width:calc(100% - 292px);min-height:100vh;display:flex;flex-direction:column}.top{min-height:62px;background:var(--top);display:flex;align-items:center;justify-content:space-between;padding:9px 18px;border-bottom:1px solid #171b20;gap:14px;font-size:12px}"
".topStatus{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.topItem{display:flex;align-items:center;gap:6px;background:#1a2025;border:1px solid var(--line);border-radius:5px;padding:7px 9px;white-space:nowrap}.topItem.ok{border-color:var(--green);color:var(--green2);font-weight:700}.topItem.bad{border-color:var(--red);color:var(--red);font-weight:700}.topActions{display:flex;align-items:center;gap:7px;flex-wrap:wrap;justify-content:flex-end}.top b{color:#fff}.tag{border:1px solid var(--green);color:var(--green2);border-radius:4px;padding:2px 7px;font-weight:700}.tag.warn{border-color:var(--amber);color:var(--amber)}.tag.bad{border-color:var(--red);color:var(--red)}"
".bufferTop{padding:5px 7px}.bufferMeter{position:relative;width:180px;height:26px;border:1px solid var(--line);border-radius:4px;overflow:hidden;background:#0e1317}.bufferFill{position:absolute;left:0;top:0;bottom:0;background:rgba(74,163,255,.45);transition:width .25s,background .3s}.bufferMark{position:absolute;top:0;bottom:0;width:2px}.bufferMark.low{background:#22e882}.bufferMark.high{background:#f0b44c}.bufferText{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-weight:700;font-size:11px;color:#fff;text-shadow:0 1px 3px rgba(0,0,0,.95);font-family:'Consolas','SF Mono','Roboto Mono',monospace;letter-spacing:.02em}"
".startBtn{background:var(--green)!important;color:#fff!important;font-weight:900;border:0!important;border-radius:6px;padding:9px 12px!important;font-family:inherit}.stopBtn{background:var(--red)!important;color:#fff!important;font-weight:900;border:0!important;border-radius:6px;padding:9px 12px!important;font-family:inherit}.refreshBtn{background:#2d3740;color:#cdd8e0;border:1px solid var(--line);border-radius:6px;padding:9px 12px;cursor:pointer;font-family:inherit;font-size:12px;font-weight:700}.refreshBtn:hover{filter:brightness(1.18)}.content{padding:26px 34px;flex:1}.crumb{font-size:12px;color:#b8c2ca;margin-bottom:13px}.crumb span{color:var(--green2)}"
".section{display:none}.section.active{display:block}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:18px}.card{background:var(--panel);border:1px solid var(--line);border-radius:5px;margin-bottom:18px}"
".card h3{font-size:14px;margin:0;padding:13px 15px;border-bottom:1px solid var(--line);font-weight:700}.cardHead{display:flex;align-items:center;justify-content:space-between;gap:12px;border-bottom:1px solid var(--line);padding:8px 15px}.cardHead h3{padding:0;border-bottom:0}.rows{padding:8px 15px 15px}.rowkv{display:grid;grid-template-columns:1.2fr 1fr;gap:10px;border-bottom:1px solid var(--line);padding:9px 0}.rowkv:last-child{border-bottom:0}.k{font-weight:700;color:#fff}.v{color:#fff;font-variant-numeric:tabular-nums}.muted{color:var(--muted)}"
".status-ok{color:var(--green2)}.status-warn{color:var(--amber)}.status-bad{color:var(--red)}button{background:#2d3740;color:#fff;border:1px solid var(--line);border-radius:5px;padding:9px 12px;margin:3px;cursor:pointer;font-family:inherit}button:hover{filter:brightness(1.12)}button.green{background:var(--green);border-color:var(--green)}button.red{background:var(--red);border-color:var(--red)}button.blue{background:#246aa5;border-color:#246aa5}button.toggle{min-width:68px;font-weight:600;transition:background .15s,border-color .15s,color .15s,opacity .15s}button.toggle.red{background:#2d1a1a;border-color:#6a2a2a;color:#f07070}button.toggle.green{background:#1a2d1a;border-color:#2a6a2a;color:#52e899}button.toggle.blue{background:#1a1a2d;border-color:#2a2a6a;color:#6ab0ff}button.toggle.black{background:#101418;border-color:#3a424a;color:#8a9499}button.toggle.active.red{background:#b03030;border-color:#d04040;color:#fff}button.toggle.active.green{background:var(--green);border-color:#1dbf73;color:#fff}button.toggle.active.blue{background:#1e5a95;border-color:#4aa3ff;color:#fff}button.toggle.active.black{background:#252c33;border-color:#5a6570;color:#dce4ea}.rateGrid{display:grid;grid-template-columns:repeat(5,minmax(70px,1fr));gap:8px;margin-bottom:9px}.rateGrid button{margin:0}"
"input,select{background:#171c21;color:#fff;border:1px solid var(--line);border-radius:5px;padding:8px;width:100%;font-family:inherit}.actions{padding:12px 15px}.pill{display:inline-block;border:1px solid var(--line);border-radius:4px;padding:3px 7px;margin:2px;color:#dce4ea}.inlineForm{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center;margin-top:10px}"
".checkrow{display:flex;align-items:center;gap:9px;padding:4px 0}.checkrow input{width:auto}.fieldrow{display:grid;grid-template-columns:1.2fr 1fr;gap:10px;align-items:center;border-bottom:1px solid var(--line);padding:9px 0}.fieldrow:last-child{border-bottom:0}.ipFields{display:none}.saveMsg{min-height:20px;color:var(--muted);padding:0 15px 12px}.saveMsg.ok{color:var(--green2)}.saveMsg.err{color:var(--red)}.note{color:var(--muted);font-size:12px;line-height:1.4;padding:0 15px 12px}"
".fileTools{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.fileTools button{margin:0}.tableWrap{overflow:auto;padding:0 15px 15px}.tableWrap table{min-width:100%}"
"table{width:100%;border-collapse:collapse}.card>table{width:calc(100% - 30px);margin:0 15px 15px}.sdTable{min-width:420px}.sdTable .sizeCol{width:92px}.sdTable .actionCol{width:78px;text-align:right}.sdTable .sizeCell{white-space:nowrap;color:#dce4ea}.sdTable .actionCell{text-align:right}.sdTable .playBtn{margin:0;padding:6px 10px}th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line)}th{font-size:12px;color:#dce4ea}.bufInputCol{width:140px}.bufInfoCol{width:120px;white-space:nowrap}.bufInfo{color:var(--muted);font-variant-numeric:tabular-nums}.bufDesc{font-size:12px}.bufActions{display:flex;align-items:center;gap:4px}.bufActions .saveMsg{padding:0 8px;margin:0}"
"pre{margin:0;background:#14191e;color:#9dffbc;padding:13px;max-height:420px;overflow:auto;font-size:12px}.footer{margin-top:auto;border-top:1px solid #171b20;background:#1a2025;text-align:center;padding:14px 16px;font-size:12px}.footerLink{color:#fff;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}.footerLink:hover{color:#fff}.footerLogo{width:112px;height:auto;display:block}"
"@media(max-width:900px){.app{display:block}.side{position:sticky;top:0;z-index:5;width:100%;padding:10px 12px;display:block;border-right:0;border-bottom:1px solid #15191d}.brand{padding:0 4px 10px}.brand h1{font-size:clamp(20px,7vw,25px)}.brand p{display:none}.nav{display:flex;gap:6px;overflow-x:auto;padding-bottom:2px}.nav button{flex:0 0 auto;width:auto;height:36px;padding:0 11px;margin:0;white-space:nowrap}.main{margin-left:0;width:100%}.top{height:auto;align-items:flex-start;flex-direction:column;padding:10px}.topActions{justify-content:flex-start}.grid2{grid-template-columns:1fr}.rateGrid{grid-template-columns:repeat(2,minmax(0,1fr))}.content{padding:16px}.rowkv,.fieldrow{grid-template-columns:1fr;gap:5px}}"
"</style>"
"</head>"
"<body>"
"<div class='app'>"

"<aside class='side'>"
"<div class='brand'><h1 id='brandName'>ILDAWaveX16</h1><p>Open-source all-in-one laser DAC</p></div>"
"<div class='nav'>"
"<button class='active' data-tab='dashboard' onclick='showTab(\"dashboard\",this)'>Dashboard</button>"
"<button data-tab='playback' onclick='showTab(\"playback\",this)'>SD Playback</button>"
"<button data-tab='protocols' onclick='showTab(\"protocols\",this)'>Protocols</button>"
"<button data-tab='network' onclick='showTab(\"network\",this)'>Network</button>"
"<button data-tab='diagnostics' onclick='showTab(\"diagnostics\",this)'>Diagnostics</button>"
"<button data-tab='system' onclick='showTab(\"system\",this)'>System</button>"
"</div>"
"</aside>"

"<main class='main'>"
"<div class='top'>"
"<div class='topStatus'>"
"<span class='topItem' id='topConn'>DAC Engine</span>"
"<span class='topItem' id='topState'>DAC Output</span>"
"<span class='topItem'><b id='topRate'>-- kpps</b></span>"
"<span class='topItem bufferTop' title='Ring buffer fill'><span class='bufferMeter'><span class='bufferFill' id='topBufferFill'></span><span class='bufferMark low' id='topBufLowMark' title='Low-water'></span><span class='bufferMark high' id='topBufHighMark' title='High-water'></span><span class='bufferText' id='topFill'>--</span></span></span>"
"</div>"
"<div class='topActions'>"
"<button class='refreshBtn' onclick='refreshEngine()' title='Refresh DAC Engine connection'>Refresh</button>"
"<button id='topStartBtn' class='startBtn' onclick='post(\"/api/rp/start\")'>START</button>"
"<button id='topStopBtn' class='stopBtn' onclick='stopAll()'>STOP</button>"
"</div>"
"</div>"

"<div class='content'>"
"<div class='crumb'>ILDAWaveX16 / <span id='crumb'>Dashboard</span></div>"

"<section id='dashboard' class='section active'>"
"<div class='grid2'>"
"<div class='card'><h3>Output health</h3><div class='rows'>"
"<div class='rowkv'><div class='k'>Underruns / second:</div><div class='v' id='dashUnderrunRate'>--</div></div>"
"<div class='rowkv'><div class='k'>Payload errors / second:</div><div class='v' id='dashPayloadRate'>--</div></div>"
"</div></div>"
"<div class='card'><h3>Protocol input</h3>"
"<div class='tableWrap'><table><thead><tr><th>Protocol</th><th>Samples / second</th><th>Status</th></tr></thead><tbody>"
"<tr><td>SD ILDA playback</td><td class='v' id='protoRateSd'>0</td><td id='protoStateSd' class='muted'>Idle</td></tr>"
"<tr><td>ILDA IDN</td><td class='v' id='protoRateIdn'>0</td><td id='protoStateIdn' class='muted'>Idle</td></tr>"
"<tr><td>Ether Dream</td><td class='v' id='protoRateEther'>0</td><td id='protoStateEther' class='muted'>Idle</td></tr>"
"<tr><td>ILW-UDP</td><td class='v' id='protoRateIlw'>0</td><td id='protoStateIlw' class='muted'>Idle</td></tr>"
"</tbody></table></div></div>"
"</div>"

"<div class='card'><h3>DAC Sample Rate</h3><div class='actions'>"
"<div class='rateGrid'>"
"<button class='blue' onclick='setRate(10000)'>10 kpps</button>"
"<button class='blue' onclick='setRate(20000)'>20 kpps</button>"
"<button class='blue' onclick='setRate(30000)'>30 kpps</button>"
"<button class='blue' onclick='setRate(60000)'>60 kpps</button>"
"<button class='blue' onclick='setRate(100000)'>100 kpps</button>"
"</div>"
"<div class='inlineForm'><input id='rateInput' type='number' value='30000' min='1' max='200000'><button onclick='setCustomRate()'>Set custom</button></div>"
"</div></div>"

"<div class='card'><h3>DAC Output Mapping</h3>"
"<div class='rows'>"
"<div class='fieldrow'><div class='k'>Mirror X</div><div class='v'><label class='checkrow'><input id='mirrorX' type='checkbox' onchange='onMirrorInput()'>Enabled</label></div></div>"
"<div class='fieldrow'><div class='k'>Mirror Y</div><div class='v'><label class='checkrow'><input id='mirrorY' type='checkbox' onchange='onMirrorInput()'>Enabled</label></div></div>"
"</div>"
"<div class='actions bufActions'><button class='green' onclick='saveMirrorSettings()'>Save</button><span id='mirrorSaveMsg' class='saveMsg'>Applies immediately and saves to DAC Engine flash.</span></div></div>"

"<div class='card'><h3>DAC Buffer</h3>"
"<table><thead><tr><th>Setting</th><th class='bufInputCol'>Value</th><th class='bufInfoCol'>Active / Limit</th><th>Description</th></tr></thead>"
"<tbody>"
"<tr><td class='k'>Buffer size</td><td><input id='bufSizeInput' inputmode='numeric' pattern='[0-9]*' oninput='onBufInput()'></td><td id='bufSizeInfo' class='bufInfo'>--</td><td class='bufDesc muted'>Total ring buffer capacity on the DAC Engine.</td></tr>"
"<tr><td class='k'>High-water</td><td><input id='bufHighInput' inputmode='numeric' pattern='[0-9]*' oninput='onBufInput()'></td><td id='bufHighInfo' class='bufInfo'>--</td><td class='bufDesc muted'>Stream pauses when fill reaches this level.</td></tr>"
"<tr><td class='k'>Low-water</td><td><input id='bufLowInput' inputmode='numeric' pattern='[0-9]*' oninput='onBufInput()'></td><td id='bufLowInfo' class='bufInfo'>--</td><td class='bufDesc muted'>Stream resumes when fill drains back to this level.</td></tr>"
"</tbody></table>"
"<div class='actions bufActions'><button class='green' onclick='saveBufferSettings()'>Save</button><button id='bufCancelBtn' onclick='cancelBufEdit()' style='display:none'>Revert</button><span id='bufSaveMsg' class='saveMsg'>Applies immediately and saves to RP flash.</span></div></div>"
"</section>"

"<section id='playback' class='section'>"
"<div class='card filesCard'><div class='cardHead'><h3>SD playback</h3><button class='refreshBtn' onclick='loadSdFiles()'>Rescan SD Card</button></div>"
"<div class='tableWrap'><table class='sdTable'><thead><tr><th>Name</th><th class='sizeCol'>Size</th><th class='actionCol'>Action</th></tr></thead><tbody id='fileRows'></tbody></table></div>"
"</div>"
"</section>"

"<section id='protocols' class='section'>"
"<div class='card'><h3>Input protocols</h3>"
"<div class='tableWrap'><table><thead><tr><th>Protocol</th><th>Enable</th><th>Status</th><th>Information</th></tr></thead><tbody>"
"<tr><td>SD ILDA playback</td><td><label class='checkrow'><input id='protoSdEnabled' type='checkbox' checked disabled>On</label></td><td class='v status-ok'>Enabled</td><td>Local microSD slot</td></tr>"
"<tr><td>ILDA IDN</td><td><label class='checkrow'><input id='protoIdnEnabled' type='checkbox'>On</label></td><td class='muted' id='protoIdnActive'>--</td><td><span id='protoIdnTransport'>UDP</span> port <span id='protoIdnPort'>--</span></td></tr>"
"<tr><td>Ether Dream</td><td><label class='checkrow'><input id='protoEtherEnabled' type='checkbox'>On</label></td><td class='muted' id='protoEtherActive'>--</td><td>TCP port <span id='protoEtherTcp'>--</span>, UDP discovery port <span id='protoEtherUdp'>--</span></td></tr>"
"<tr><td>ILW-UDP</td><td><label class='checkrow'><input id='protoIlwEnabled' type='checkbox'>On</label></td><td class='muted' id='protoIlwActive'>--</td><td><span id='protoIlwTransport'>UDP</span> port <span id='protoIlwPort'>--</span></td></tr>"
"</tbody></table></div>"
"<div class='actions'><button class='green' onclick='saveProtocolSettings()'>Save protocol settings</button><button onclick='restartDevice()'>Reboot</button></div><div id='protoSaveMsg' class='saveMsg'>Settings are saved to flash and applied on boot.</div></div>"
"</section>"

"<section id='network' class='section'>"
"<div class='grid2'>"
"<div class='card'><h3>Ethernet / LAN</h3><div class='rows'>"
"<div class='rowkv'><div class='k'>Ethernet link:</div><div class='v' id='netEthState'>--</div></div>"
"<div class='rowkv'><div class='k'>Ethernet IP:</div><div class='v' id='netEthIp'>--</div></div>"
"<div class='fieldrow'><label class='k' for='netHostname'>Hostname:</label><div class='v'><input id='netHostname' maxlength='31'></div></div>"
"<div class='fieldrow'><label class='k' for='netIpMode'>IP address:</label><div class='v'><select id='netIpMode' onchange='updateIpFields()'><option value='dhcp'>Automatic (DHCP)</option><option value='static'>Manual (static)</option></select></div></div>"
"<div id='netIpFields' class='ipFields'>"
"<div class='fieldrow'><label class='k' for='netStaticIp'>Static IP:</label><div class='v'><input id='netStaticIp' inputmode='numeric'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaticMask'>Netmask:</label><div class='v'><input id='netStaticMask' inputmode='numeric'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaticGateway'>Gateway:</label><div class='v'><input id='netStaticGateway' inputmode='numeric'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaticDns'>DNS server:</label><div class='v'><input id='netStaticDns' inputmode='numeric'></div></div>"
"</div>"
"</div></div>"
"<div class='card'><h3>Wi-Fi access point</h3><div class='rows'>"
"<div class='fieldrow'><div class='k'>Access point:</div><div class='v'><label class='checkrow'><input id='netApEnabled' type='checkbox'>Enabled</label></div></div>"
"<div class='rowkv'><div class='k'>AP IP:</div><div class='v' id='netApIp'>--</div></div>"
"<div class='fieldrow'><label class='k' for='netApSsid'>AP name:</label><div class='v'><input id='netApSsid' maxlength='32'></div></div>"
"<div class='fieldrow'><label class='k' for='netApPass'>AP password:</label><div class='v'><input id='netApPass' type='password' maxlength='63' placeholder='Leave blank to keep current'></div></div>"
"</div></div>"
"</div>"
"<div class='grid2'>"
"<div class='card'><h3>Wi-Fi station</h3><div class='rows'>"
"<div class='fieldrow'><div class='k'>Station mode:</div><div class='v'><label class='checkrow'><input id='netStaEnabled' type='checkbox'>Enabled</label></div></div>"
"<div class='rowkv'><div class='k'>Station status:</div><div class='v' id='netStaState'>--</div></div>"
"<div class='rowkv'><div class='k'>Station IP:</div><div class='v' id='netStaIp'>--</div></div>"
"<div class='fieldrow'><label class='k' for='netStaSsid'>Network name:</label><div class='v'><input id='netStaSsid' maxlength='32'></div></div>"
"<div class='fieldrow'><label class='k' for='netStaPass'>Network password:</label><div class='v'><input id='netStaPass' type='password' maxlength='63' placeholder='Leave blank to keep current'></div></div>"
"</div></div>"
"<div class='card'><h3>Apply settings</h3><div class='actions'>"
"<button class='green' onclick='saveNetworkSettings()'>Save network settings</button>"
"<button onclick='restartDevice()'>Reboot</button>"
"</div><div id='netSaveMsg' class='saveMsg'>Settings are saved to flash and applied on boot.</div>"
"<div class='note'>Changing Wi-Fi or IP settings can move this page to a new address after reboot.</div></div>"
"</div>"
"</section>"

"<section id='diagnostics' class='section'>"
"<div class='card'><h3>Diagnostic counters</h3>"
"<div class='tableWrap'><table><thead><tr><th>Name</th><th>Number</th><th>Information</th></tr></thead><tbody>"
"<tr><td>Stream packets</td><td class='v' id='diagPackets'>--</td><td class='muted'>Valid SPI sample packets sent from the ESP32 to the DAC Engine.</td></tr>"
"<tr><td>Payload errors</td><td class='v' id='diagPayloadErrors'>--</td><td class='muted'>Payload checksum or framing errors reported by the DAC Engine.</td></tr>"
"<tr><td>Underruns</td><td class='v' id='diagUnderruns'>--</td><td class='muted'>Times the DAC Engine needed a sample but its buffer was empty.</td></tr>"
"<tr><td>Bad packets</td><td class='v' id='diagBadPackets'>--</td><td class='muted'>Malformed stream packets rejected before use.</td></tr>"
"<tr><td>Dropped packets</td><td class='v' id='diagDroppedPackets'>--</td><td class='muted'>Packets dropped because the DAC Engine could not accept them.</td></tr>"
"<tr><td>Idle RX wait timeouts</td><td class='v' id='diagRxTimeouts'>--</td><td class='muted'>Times the DAC Engine receiver armed itself for SPI input while no ESP32 stream packet arrived. This can increase while DAC output is stopped.</td></tr>"
"</tbody></table></div><div class='actions'><button onclick='resetDiagnosticCounters()'>Reset counters</button></div></div>"
"<div class='grid2'>"
"<div class='card'><h3>Status JSON</h3><pre id='status'>Loading...</pre></div>"
"<div class='card'><h3>Test Stream</h3>"
"<div class='actions'>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=square\")'>Square</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=circle\")'>Circle</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=cross\")'>Cross</button>"
"<button onclick='post(\"/api/rp/start_test_stream?shape=heart\")'>Heart</button>"
"</div>"
"<div class='actions' style='padding-top:0'>"
"<button id='testRedBtn' class='toggle red' onclick='toggleTestColor(1)'>Red</button>"
"<button id='testGreenBtn' class='toggle green' onclick='toggleTestColor(2)'>Green</button>"
"<button id='testBlueBtn' class='toggle blue' onclick='toggleTestColor(4)'>Blue</button>"
"<button id='testBlackBtn' class='toggle black' onclick='setTestBlack()'>Black</button>"
"</div>"
"<div class='actions' style='padding-top:0'>"
"<button class='red' onclick='post(\"/api/rp/stop_test_stream\")'>Stop Stream</button>"
"</div></div>"
"</div>"
"</section>"

"<section id='system' class='section'>"
"<div class='grid2'>"
"<div class='card'><h3>Firmware</h3><div class='rows'>"
"<div class='rowkv'><div class='k'>Main (ESP32-S3):</div><div class='v' id='aboutEspFw'>--</div></div>"
"<div class='rowkv'><div class='k'>DAC Engine (RP2350):</div><div class='v' id='aboutRpFw'>--</div></div>"
"</div></div>"
"<div class='card'><h3>Maintenance</h3><div class='rows'>"
"</div><div class='actions'><button class='red' onclick='factoryReset()'>Factory Reset</button><button onclick='restartDevice()'>Reboot</button></div><div id='systemMsg' class='saveMsg'></div></div>"
"</div>"
"</section>"
"</div>"
"<div class='footer'><a class='footerLink' href='https://stanleyprojects.com' target='_blank' rel='noopener' aria-label='StanleyProjects'><svg class='footerLogo' xmlns='http://www.w3.org/2000/svg' width='400' height='102.7' fill='currentColor' viewBox='0 0 300 77' aria-hidden='true'><path d='M.7.7H300V77H.8zm6.8 6.8V70h285.3V7.5zM23 22c2.2-4.8 9.7-6.3 13.3-2.3 2.4 4 .5 8.8.2 13.1h-6.6c.5-3.5 1.4-7 .3-10.5q-3 5-2.4 10.8c1.8 3.8 7.6 5 7.4 9.8-.2 4.5-.6 9-2 13.3-2.3 5.5-11.8 7.2-14.6 1.5-1.3-4.6.3-9.3.8-13.8H26c-.5 3.7-1.4 7.5-1.2 11.3 1.7.9 2.4-1.1 2.8-2.3.6-3 1.2-6 .8-9-1.8-3.7-7.1-5.2-7.2-9.9 0-4 .2-8.2 1.9-12m18-3.9h15.7l-.7 5.7-4.6.1-5.2 36.6h-6.6L44.8 24h-4.6zm19.2 0h13.5l-3.5 42.4h-6.5q.7-6.4 1.1-13l-4 .1-2.4 13h-6.7zM62 41.7h3.4c.2-6.3 2.4-12.8.7-19.1-2.3 6.1-2.8 12.7-4.1 19m16-23.5 6.5-.1L86 36q1.5-9 2.7-18H95l-5.8 42.4h-6.5L81 41.6q-1.4 9.4-2.7 19l-6.3-.1zm13.9 42.3 5.9-42.4h6.7l-5 36.5h4.8l-.8 6zm19-42.4h13l-.7 5.8h-6.5l-1.4 10.7h4.9l-.8 6h-5l-2 14h6.6c-.5 1.9-.4 4-1.3 5.8-4.2.3-8.5 0-12.7 0zm14.3 0h6.5q.2 6.1.8 12.2l3.7-12.2h6.2l-7 19.3c-3 7.3-3 15.4-4.4 23l-6.7.1c.9-7.8 3-15.6 2.2-23.5zm19 .2c4.2-.3 8.7-1.3 12.7.6 2.6 1.8 2.3 5.4 2.1 8.2-.5 4.5-1 9.4-3.7 13.2-2 2.4-5.2 2.5-8 3L145 60.5h-6.7zm4.1 19.6c4-3.8 5-10.4 2.5-15.2-1.7 4.9-2 10-2.5 15.2m14.3-19.6c4-.3 8.4-1.3 12.2.7 3.5 2.2 2.9 6.8 2.4 10.3-.2 3.1-1.4 6.2-3.7 8.4.9 2 2 4 1.7 6.2-.5 5.6-1.6 11-2.3 16.6h-6.6c.7-5.5 1.8-11 2.1-16.5.4-1.7-1-2.8-2.3-3.4l-2.8 20-6.7-.1zM167 36c3.8-3 5-9.5 1.9-13.2q-1.5 6.5-1.9 13.2m18.2-17.5c4-2.3 11-1 11.2 4.5-.4 9-2.4 18-3.4 27-.3 4.2-1.7 9.1-6.2 10.6-4.1 1.9-10.5-.3-10.2-5.5.5-8.4 2.3-16.7 3.2-25.1.5-4.2 1-9.5 5.4-11.5m2.4 5.8c-1.4 5.1-1.6 10.5-2.5 15.7-.6 5.3-2.2 10.7-1.4 16 2.8-2.1 2.5-6 3.1-9.2.8-7.3 2.4-14.5 2.9-21.8q-.3-3.4-2.1-.7m16.5-.4 1.6-5.8h10.4l-5 34.9c-.4 3.8-3.2 7.5-7.2 8-3.5 1-8.2-1-8.4-5-.3-4.8 1-9.6 1.4-14.4h6.8c-.7 4.7-1.6 9.4-1.6 14.2 1.3-1 2.4-2 2.4-3.7l4-28.2zm8.8 36.6 5.9-42.4h13l-.7 5.8h-6.5l-1.5 10.6h5l-.8 6.1h-5l-2 14h6.5l-1 6zm25.2-42.2c3.8-1.8 9.9-1 10.8 3.8.5 4.8-1 9.6-1.4 14.4H241c.6-4.7 1.6-9.3 1.2-14-3.2 2-2.7 6.2-3.3 9.5-.8 7.4-2.5 14.6-2.9 22l1 1.8c3-4 2-9.5 3.3-14.2h6.5c-.7 4.7-.9 9.6-2.3 14-1.9 5-8.8 7.4-13.1 4.1-2.7-2-2.2-5.8-1.8-8.7l3.3-24c.4-3.5 2-7.2 5.3-8.7m13.9-.1 15.8-.1-.8 5.7-4.6.1-5.1 36.6h-6.6l5.1-36.6h-4.6zm17.5 4.6c1.7-5.4 9.9-7.3 13.6-3 2.4 4 .5 8.8.2 13h-6.6q1-5 1-10c-3.2 1.4-2.6 5.4-3.1 8.3-.7 5.2 7 6 7.4 10.9q0 6.8-1.8 13.5c-1.8 5.8-11 8-14.5 2.8-2-4.6.1-9.7.5-14.4h6.5c-.5 3.8-1.3 7.5-1.1 11.3 1.6.7 2.3-1 2.7-2.2.6-3 1.2-6 .8-9-1.8-3.8-7-5.3-7.2-10q-.1-5.7 1.6-11.2'/></svg></a></div>"
"</main></div>"

"<script>"
"let prev=null;let rates={underruns:0,payload:0,packets:0,bad:0,dropped:0,rxTimeouts:0,sd:0,idn:0,ether:0,ilw:0};let bufDirty=false;let mirrorDirty=false;let lastRpStatus=null;let lastSdSig=null;let diagBase=null;let testColorMask=7;"
"function showTab(id,btn){btn=btn||document.querySelector('.nav button[data-tab='+id+']');document.querySelectorAll('.section').forEach(s=>s.classList.remove('active'));document.getElementById(id).classList.add('active');document.querySelectorAll('.nav button').forEach(b=>b.classList.remove('active'));if(btn)btn.classList.add('active');document.getElementById('crumb').innerText=btn?btn.innerText:id;if(id==='playback')loadSdFiles();if(id==='protocols')loadProtocolSettings();if(id==='network')loadNetworkSettings();}"
"function post(url){return fetch(url,{method:'POST'}).then(r=>r.text()).then(t=>{console.log(url,t);setTimeout(update,120);return t;});}"
"function refreshEngine(){return post('/api/scan/refresh');}"
"function stopAll(){return post('/api/rp/stop');}"
"function setRate(r){return post('/api/rp/set_rate?value='+encodeURIComponent(r));}"
"function setCustomRate(){setRate(document.getElementById('rateInput').value);}"
"function syncTestColorButtons(){['testRedBtn','testGreenBtn','testBlueBtn','testBlackBtn'].forEach(id=>{let e=document.getElementById(id);if(e)e.classList.remove('active');});let r=document.getElementById('testRedBtn');let g=document.getElementById('testGreenBtn');let b=document.getElementById('testBlueBtn');let k=document.getElementById('testBlackBtn');if((testColorMask&1)&&r)r.classList.add('active');if((testColorMask&2)&&g)g.classList.add('active');if((testColorMask&4)&&b)b.classList.add('active');if(testColorMask===0&&k)k.classList.add('active');}"
"function sendTestColor(){syncTestColorButtons();return post('/api/rp/test_color?mask='+encodeURIComponent(testColorMask));}"
"function toggleTestColor(bit){testColorMask^=bit;sendTestColor();}"
"function setTestBlack(){testColorMask=0;sendTestColor();}"
"function onMirrorInput(){mirrorDirty=true;setMirrorMsg('Unsaved changes.','');}"
"function saveMirrorSettings(){let p=new URLSearchParams();p.set('mirror_x',checked('mirrorX')?'1':'0');p.set('mirror_y',checked('mirrorY')?'1':'0');setMirrorMsg('Saving to DAC Engine flash...','ok');return fetch('/api/rp/mirror_config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok||!t.startsWith('OK')){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}mirrorDirty=false;setMirrorMsg('Saved and applied.','ok');setTimeout(update,120);return t;})).catch(e=>setMirrorMsg(e.message||'Save failed','err'));}"
"function onBufInput(){bufDirty=true;document.getElementById('bufCancelBtn').style.display='';setBufMsg('Unsaved changes.','');}"
"function cancelBufEdit(){bufDirty=false;document.getElementById('bufCancelBtn').style.display='none';setBufMsg('Applies immediately and saves to RP flash.','');if(lastRpStatus)forceSyncBufInputs(lastRpStatus);}"
"function defaultHigh(size){return size>2048?size-2048:Math.max(256,size-256);}"
"function defaultLow(high){return high>2048?high-2048:Math.max(0,high-256);}"
"function forceSyncBufInputs(j){let size=Number(j.remote_buffer_size_samples||0);let high=Number(j.remote_buffer_high_water_samples||0);let low=Number(j.remote_buffer_low_water_samples||0);if(size&&!high)high=defaultHigh(size);setVal('bufSizeInput',size||'');setVal('bufHighInput',high||'');setVal('bufLowInput',low);}"
"function saveBufferSettings(){let size=Number(val('bufSizeInput'));let high=Number(val('bufHighInput'));let low=Number(val('bufLowInput'));let max=lastRpStatus?Number(lastRpStatus.remote_buffer_max_samples||0):0;if(!Number.isInteger(size)||!Number.isInteger(high)||!Number.isInteger(low)||size<512||high<256||low<0){setBufMsg('Enter valid whole-number buffer limits','err');return;}if(max&&size>max){setBufMsg('Buffer size is above the RP maximum','err');return;}if(high>size-256){setBufMsg('High-water limit must leave at least 256 samples of headroom','err');return;}if(low>high-256){setBufMsg('Low-water limit must be at least 256 samples below high-water','err');return;}let p=new URLSearchParams();p.set('size',size);p.set('high_water',high);p.set('low_water',low);setBufMsg('Saving to RP flash...','ok');return fetch('/api/rp/buffer_config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok||!t.startsWith('OK')){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}setBufMsg('Saved and applied.','ok');bufDirty=false;document.getElementById('bufCancelBtn').style.display='none';setTimeout(update,120);return t;})).catch(e=>setBufMsg(e.message||'Save failed','err'));}"
"function playIldaPath(p){post('/api/ilda/start?path='+encodeURIComponent(p));}"
"function val(id){let e=document.getElementById(id);return e?e.value:'';}"
"function setVal(id,v){let e=document.getElementById(id);if(e)e.value=(v===0||v)?v:'';}"
"function setValIfIdle(id,v){let e=document.getElementById(id);if(e&&document.activeElement!==e)e.value=(v===0||v)?v:'';}"
"function checked(id){let e=document.getElementById(id);return !!(e&&e.checked);}"
"function setChecked(id,v){let e=document.getElementById(id);if(e)e.checked=!!v;}"
"function updateIpFields(){let e=document.getElementById('netIpFields');if(e)e.style.display=val('netIpMode')==='static'?'block':'none';}"
"function setNetMsg(t,kind){let e=document.getElementById('netSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setProtoMsg(t,kind){let e=document.getElementById('protoSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setSystemMsg(t,kind){let e=document.getElementById('systemMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setBufMsg(t,kind){let e=document.getElementById('bufSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function setMirrorMsg(t,kind){let e=document.getElementById('mirrorSaveMsg');if(e){e.innerText=t;e.className='saveMsg '+(kind||'');}}"
"function loadProtocolSettings(){return fetch('/api/protocols/config').then(r=>r.json()).then(j=>{setChecked('protoSdEnabled',true);setChecked('protoIdnEnabled',j.idn_enabled);setChecked('protoEtherEnabled',j.etherdream_enabled);setChecked('protoIlwEnabled',j.ilw_enabled);setText('protoIdnTransport',j.idn_transport||'UDP');setText('protoIdnPort',j.idn_udp_port);setText('protoEtherTcp',j.etherdream_tcp_port);setText('protoEtherUdp',j.etherdream_udp_port);setText('protoIlwTransport',j.ilw_transport||'UDP');setText('protoIlwPort',j.ilw_udp_port);let a=j.active||{};setText('protoIdnActive',a.idn_enabled?'Enabled':'Disabled');setText('protoEtherActive',a.etherdream_enabled?'Enabled':'Disabled');setText('protoIlwActive',a.ilw_enabled?'Enabled':'Disabled');setClass('protoIdnActive',a.idn_enabled?'v status-ok':'v status-warn');setClass('protoEtherActive',a.etherdream_enabled?'v status-ok':'v status-warn');setClass('protoIlwActive',a.ilw_enabled?'v status-ok':'v status-warn');setProtoMsg(j.reboot_required?'Saved settings pending restart':'Settings are saved to flash and applied on boot.',j.reboot_required?'ok':'');}).catch(e=>setProtoMsg('Protocol settings load failed','err'));}"
"function saveProtocolSettings(){let p=new URLSearchParams();p.set('sd_ilda_enabled','1');p.set('idn_enabled',checked('protoIdnEnabled')?'1':'0');p.set('etherdream_enabled',checked('protoEtherEnabled')?'1':'0');p.set('ilw_enabled',checked('protoIlwEnabled')?'1':'0');return fetch('/api/protocols/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}setProtoMsg('Saved. Restart device to apply.','ok');loadProtocolSettings();return t;})).catch(e=>setProtoMsg(e.message||'Save failed','err'));}"
"function loadNetworkSettings(){return fetch('/api/network/config').then(r=>r.json()).then(j=>{setChecked('netApEnabled',j.wifi_ap_enabled);setVal('netApSsid',j.wifi_ap_ssid);setVal('netApPass','');setChecked('netStaEnabled',j.wifi_sta_enabled);setVal('netStaSsid',j.wifi_sta_ssid);setVal('netStaPass','');setVal('netHostname',j.hostname);setVal('netIpMode',j.ip_mode);setVal('netStaticIp',j.static_ip);setVal('netStaticMask',j.static_netmask);setVal('netStaticGateway',j.static_gateway);setVal('netStaticDns',j.static_dns);updateIpFields();let s=j.status||{};setText('netEthState',s.eth_link?(s.eth_has_ip?'Ready':'Link only'):'Disconnected');setText('netEthIp',s.eth_has_ip?s.eth_ip:'--');setText('netApIp',s.wifi_ap_enabled?s.wifi_ap_ip:'Disabled');setText('netStaState',!j.wifi_sta_enabled?'Disabled':(s.wifi_sta_connected?'Connected':'Connecting'));setText('netStaIp',s.wifi_sta_connected?s.wifi_sta_ip:'--');setNetMsg(j.reboot_required?'Saved settings pending restart':'Settings are saved to flash and applied on boot.',j.reboot_required?'ok':'');}).catch(e=>setNetMsg('Network settings load failed','err'));}"
"function saveNetworkSettings(){let p=new URLSearchParams();let apPass=val('netApPass');let staPass=val('netStaPass');p.set('ap_enabled',checked('netApEnabled')?'1':'0');p.set('ap_ssid',val('netApSsid'));if(apPass)p.set('ap_password',apPass);p.set('sta_enabled',checked('netStaEnabled')?'1':'0');p.set('sta_ssid',val('netStaSsid'));if(staPass)p.set('sta_password',staPass);p.set('hostname',val('netHostname'));p.set('ip_mode',val('netIpMode'));p.set('static_ip',val('netStaticIp'));p.set('static_netmask',val('netStaticMask'));p.set('static_gateway',val('netStaticGateway'));p.set('static_dns',val('netStaticDns'));return fetch('/api/network/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(r=>r.text().then(t=>{if(!r.ok){throw new Error(t.startsWith('ERR ')?t.substring(4):t);}setNetMsg('Saved. Restart device to apply.','ok');loadNetworkSettings();return t;})).catch(e=>setNetMsg(e.message||'Save failed','err'));}"
"function restartDevice(){if(!confirm('Restart device now?'))return;fetch('/api/system/reboot',{method:'POST'}).then(()=>{setNetMsg('Restarting...','ok');setProtoMsg('Restarting...','ok');setSystemMsg('Restarting...','ok');}).catch(e=>{setNetMsg('Restart failed','err');setProtoMsg('Restart failed','err');setSystemMsg('Restart failed','err');});}"
"function factoryReset(){if(!confirm('Restore default settings and restart now?'))return;fetch('/api/system/factory_reset',{method:'POST'}).then(r=>r.text().then(t=>{if(!r.ok)throw new Error(t.startsWith('ERR ')?t.substring(4):t);setSystemMsg('Factory defaults restored. Restarting...','ok');setNetMsg('Factory defaults restored. Restarting...','ok');setProtoMsg('Factory defaults restored. Restarting...','ok');})).catch(e=>setSystemMsg(e.message||'Factory reset failed','err'));}"
"function fmtBytes(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB';}"
"function fmtKpps(pps){return (pps/1000).toFixed(0)+' kpps';}"
"function fmtRate(v){return v>=1000?(v/1000).toFixed(1)+' k/s':v.toFixed(0);}"
"function fmtBuffer(j){return j.remote_buffer_size_samples?j.remote_buffer_fill_samples+'/'+j.remote_buffer_size_samples:j.remote_buffer_fill_samples;}"
"function loadSdFiles(){fetch('/api/sd/list').then(r=>r.json()).then(j=>{let body=document.getElementById('fileRows');if(!body)return;body.innerHTML='';if(!j.files)return;j.files.forEach(f=>{if(f.type==='dir'||f.type==='directory'||f.is_dir)return;let tr=document.createElement('tr');let playable=f.name.toLowerCase().endsWith('.ild');tr.innerHTML='<td>'+f.name+'</td><td class=\"sizeCell\">'+fmtBytes(f.size)+'</td><td class=\"actionCell\">'+(playable?'<button class=\"green playBtn\">Play</button>':'<span class=\"muted\">--</span>')+'</td>';if(playable){tr.children[2].querySelector('button').onclick=()=>playIldaPath(f.path);}body.appendChild(tr);});}).catch(e=>{let body=document.getElementById('fileRows');if(body)body.innerHTML='';});}"
"function syncSdFiles(j){let sig=[j.sd_inserted?1:0,j.sd_mounted?1:0,j.sd_file_count||0].join(':');if(sig!==lastSdSig){lastSdSig=sig;loadSdFiles();}}"
"function syncProtocolRates(){setText('protoRateSd',fmtRate(rates.sd));setText('protoRateIdn',fmtRate(rates.idn));setText('protoRateEther',fmtRate(rates.ether));setText('protoRateIlw',fmtRate(rates.ilw));setText('protoStateSd',rates.sd>0?'Active':'Idle');setText('protoStateIdn',rates.idn>0?'Active':'Idle');setText('protoStateEther',rates.ether>0?'Active':'Idle');setText('protoStateIlw',rates.ilw>0?'Active':'Idle');setClass('protoStateSd',rates.sd>0?'v status-ok':'muted');setClass('protoStateIdn',rates.idn>0?'v status-ok':'muted');setClass('protoStateEther',rates.ether>0?'v status-ok':'muted');setClass('protoStateIlw',rates.ilw>0?'v status-ok':'muted');}"
"function countFromBase(j,k){let b=diagBase?Number(diagBase[k]||0):0;return Math.max(0,Number(j[k]||0)-b);}"
"function syncDiagnosticTotals(j){setText('diagPackets',countFromBase(j,'remote_stream_packets'));setText('diagPayloadErrors',countFromBase(j,'remote_payload_errors'));setText('diagUnderruns',countFromBase(j,'remote_underruns'));setText('diagBadPackets',countFromBase(j,'remote_stream_bad_packets'));setText('diagDroppedPackets',countFromBase(j,'remote_stream_dropped_packets'));setText('diagRxTimeouts',countFromBase(j,'remote_stream_rx_timeouts'));}"
"function resetDiagnosticCounters(){if(!lastRpStatus)return;diagBase={remote_stream_packets:Number(lastRpStatus.remote_stream_packets||0),remote_payload_errors:Number(lastRpStatus.remote_payload_errors||0),remote_underruns:Number(lastRpStatus.remote_underruns||0),remote_stream_bad_packets:Number(lastRpStatus.remote_stream_bad_packets||0),remote_stream_dropped_packets:Number(lastRpStatus.remote_stream_dropped_packets||0),remote_stream_rx_timeouts:Number(lastRpStatus.remote_stream_rx_timeouts||0)};syncDiagnosticTotals(lastRpStatus);}"
"function calcRates(j){let now=Date.now();if(prev){let dt=(now-prev.t)/1000;if(dt>0){rates.underruns=Math.max(0,(j.remote_underruns-prev.underruns)/dt);rates.payload=Math.max(0,(j.remote_payload_errors-prev.payload)/dt);rates.packets=Math.max(0,(j.remote_stream_packets-prev.packets)/dt);rates.bad=Math.max(0,(j.remote_stream_bad_packets-prev.bad)/dt);rates.dropped=Math.max(0,(j.remote_stream_dropped_packets-prev.dropped)/dt);rates.rxTimeouts=Math.max(0,(j.remote_stream_rx_timeouts-prev.rxTimeouts)/dt);rates.sd=Math.max(0,((j.stream_sd_ilda_samples||0)-prev.sd)/dt);rates.idn=Math.max(0,((j.stream_idn_samples||0)-prev.idn)/dt);rates.ether=Math.max(0,((j.stream_etherdream_samples||0)-prev.ether)/dt);rates.ilw=Math.max(0,((j.stream_ilw_samples||0)-prev.ilw)/dt);}}prev={t:now,underruns:j.remote_underruns,payload:j.remote_payload_errors,packets:j.remote_stream_packets,bad:j.remote_stream_bad_packets,dropped:j.remote_stream_dropped_packets,rxTimeouts:j.remote_stream_rx_timeouts,sd:j.stream_sd_ilda_samples||0,idn:j.stream_idn_samples||0,ether:j.stream_etherdream_samples||0,ilw:j.stream_ilw_samples||0};}"
"function resetUiCounters(){prev=null;rates={underruns:0,payload:0,packets:0,bad:0,dropped:0,rxTimeouts:0,sd:0,idn:0,ether:0,ilw:0};syncProtocolRates();}"
"function setText(id,v){let e=document.getElementById(id);if(e)e.innerText=v;}"
"function setClass(id,v){let e=document.getElementById(id);if(e)e.className=v;}"
"function setButtonState(running){}"
"function diagnosticStatus(j){return {product:j.product,esp_fw:j.esp_fw,rp_fw:j.rp_fw,dac_engine_ready:!!j.initialized,dac_running:!!j.remote_running,sample_rate:j.remote_sample_rate,buffer_fill:j.remote_buffer_fill_samples,buffer_size:j.remote_buffer_size_samples,buffer_high_water:j.remote_buffer_high_water_samples,buffer_low_water:j.remote_buffer_low_water_samples,buffer_max:j.remote_buffer_max_samples,mirror_x:!!j.remote_mirror_x,mirror_y:!!j.remote_mirror_y,underruns:j.remote_underruns,payload_errors:j.remote_payload_errors,stream_packets:j.remote_stream_packets,bad_packets:j.remote_stream_bad_packets,dropped_packets:j.remote_stream_dropped_packets,rx_timeouts:j.remote_stream_rx_timeouts,sd_ilda_samples:j.stream_sd_ilda_samples,idn_samples:j.stream_idn_samples,etherdream_samples:j.stream_etherdream_samples,ilw_samples:j.stream_ilw_samples,sd_ilda_running:!!j.ilda_running,test_pattern_running:!!j.test_stream_running};}"
"function syncMirrorFields(j){if(!mirrorDirty){setChecked('mirrorX',!!j.remote_mirror_x);setChecked('mirrorY',!!j.remote_mirror_y);}}"
"function syncBufferFields(j){let rpMax=Number(j.remote_buffer_max_samples||0);let rpSize=Number(j.remote_buffer_size_samples||0);let rpHigh=Number(j.remote_buffer_high_water_samples||0);let rpLow=Number(j.remote_buffer_low_water_samples||0);let sizeLimit=rpMax||rpSize;let highLimit=rpSize>256?rpSize-256:0;if(rpSize&&!rpHigh)rpHigh=defaultHigh(rpSize);let lowLimit=rpHigh>256?rpHigh-256:0;setText('bufSizeInfo',rpSize?(rpSize+' / '+sizeLimit):'--');setText('bufHighInfo',rpHigh?(rpHigh+' / '+highLimit):'--');setText('bufLowInfo',rpHigh?(rpLow+' / '+lowLimit):'--');if(!bufDirty){setValIfIdle('bufSizeInput',rpSize||'');setValIfIdle('bufHighInput',rpHigh||'');setValIfIdle('bufLowInput',rpLow);}}"
"function updateBufferMeter(j){let size=Number(j.remote_buffer_size_samples||0);let fill=Number(j.remote_buffer_fill_samples||0);let low=Number(j.remote_buffer_low_water_samples||0);let high=Number(j.remote_buffer_high_water_samples||0);let lf=size?Math.max(0,Math.min(100,low*100/size)):0;let hf=size?Math.max(lf,Math.min(100,high*100/size)):0;let ff=size?Math.max(0,Math.min(100,fill*100/size)):0;let f=document.getElementById('topBufferFill');let lm=document.getElementById('topBufLowMark');let hm=document.getElementById('topBufHighMark');if(f){f.style.width=ff+'%';f.style.background=!size?'rgba(74,163,255,.35)':fill>=high?'rgba(217,155,40,.55)':fill<low?'rgba(29,191,115,.5)':'rgba(74,163,255,.45)';}if(lm)lm.style.left=lf+'%';if(hm)hm.style.left=hf+'%';}"
"function render(j){lastRpStatus=j;calcRates(j);setButtonState(j.remote_running);syncBufferFields(j);syncMirrorFields(j);syncSdFiles(j);syncProtocolRates();syncDiagnosticTotals(j);updateBufferMeter(j);let dacEngineOk=!!j.initialized;let dacRun=!!j.remote_running;let bufferText=fmtBuffer(j);if(j.product)document.title=j.product;setText('brandName',j.product);setText('aboutEspFw',j.esp_fw);setText('aboutRpFw',j.rp_fw);setText('topRate',fmtKpps(j.remote_sample_rate));setText('topFill',bufferText);setClass('topConn',dacEngineOk?'topItem ok':'topItem bad');setClass('topState',dacRun?'topItem ok':'topItem bad');setText('dashUnderrunRate',rates.underruns.toFixed(1));setText('dashPayloadRate',rates.payload.toFixed(1));setClass('dashUnderrunRate',rates.underruns>0?'v status-warn':'v status-ok');setClass('dashPayloadRate',rates.payload>0?'v status-bad':'v status-ok');document.getElementById('status').innerText=JSON.stringify(diagnosticStatus(j),null,2);}"
"function update(){fetch('/api/rp/status',{cache:'no-store'}).then(r=>r.json()).then(render).catch(e=>{setClass('topConn','topItem bad');});}"
"function boot(){syncTestColorButtons();loadNetworkSettings();loadProtocolSettings();refreshEngine().finally(()=>{update();});}"
"setInterval(update,2000);boot();"
"</script>"
"</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    set_common_response_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_portal_get_handler(httpd_req_t *req)
{
    if (strncmp(req->uri, "/api/", 5) == 0) {
        set_common_response_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Not found");
    }

    set_common_response_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Redirecting to ILDAWaveX16 setup");
}

/* ===================== REGISTRATION ===================== */

static esp_err_t register_handler(const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t uri_config = {
        .uri = path,
        .method = method,
        .handler = handler,
        .user_ctx = NULL
    };

    return httpd_register_uri_handler(s_server, &uri_config);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 44;
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.backlog_conn = 4;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    config.stack_size = WEB_SERVER_TASK_STACK_BYTES;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting web server");

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(register_handler("/", HTTP_GET, root_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/sd/list", HTTP_GET, sd_list_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/network/config", HTTP_GET, network_config_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/network/config", HTTP_POST, network_config_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/protocols/config", HTTP_GET, protocol_config_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/protocols/config", HTTP_POST, protocol_config_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/system/reboot", HTTP_POST, system_reboot_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/system/factory_reset", HTTP_POST, system_factory_reset_post_handler));

    ESP_ERROR_CHECK(register_handler("/api/rp/status", HTTP_GET, rp_status_get_handler));
    ESP_ERROR_CHECK(register_handler("/api/scan/refresh", HTTP_POST, rp_refresh_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/ping", HTTP_POST, rp_ping_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/start", HTTP_POST, rp_start_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/stop", HTTP_POST, rp_stop_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/clear", HTTP_POST, rp_clear_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/start_test_stream", HTTP_POST, rp_start_stream_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/test_color", HTTP_POST, rp_test_color_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/stop_test_stream", HTTP_POST, rp_stop_stream_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/set_rate", HTTP_POST, rp_set_rate_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/buffer_config", HTTP_POST, rp_buffer_config_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/rp/mirror_config", HTTP_POST, rp_mirror_config_post_handler));

    ESP_ERROR_CHECK(register_handler("/api/ilda/start", HTTP_POST, ilda_start_post_handler));
    ESP_ERROR_CHECK(register_handler("/api/ilda/stop", HTTP_POST, ilda_stop_post_handler));
    ESP_ERROR_CHECK(register_handler("/*", HTTP_GET, captive_portal_get_handler));

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            dac_engine_refresh_task,
            "dac_refresh",
            4096,
            NULL,
            2,
            &s_refresh_task,
            0
        );

        if (ok != pdPASS) {
            ESP_LOGW(TAG, "Failed to start scan refresh task");
        }
    }

    return ESP_OK;
}
