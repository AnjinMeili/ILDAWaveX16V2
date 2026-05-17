#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "nvs.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "net_manager.h"

#if ENABLE_MDNS
#include "mdns.h"
#endif

#include "driver/gpio.h"
#include "driver/spi_master.h"

#if BOARD_VERSION == 2
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#endif

static const char *TAG = "net";

static const char *NET_NVS_NAMESPACE = "netcfg";

#if BOARD_VERSION == 2
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
#endif
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static TaskHandle_t s_captive_dns_task = NULL;
static esp_ip4_addr_t s_captive_dns_ip;
static char s_captive_portal_uri[NET_HOSTNAME_MAX_LEN + 16];

static net_settings_t s_settings;
static net_settings_t s_active_settings;
static bool s_settings_loaded = false;

static net_status_t s_status = {
#if BOARD_VERSION == 2
    .eth_link = false,
    .eth_has_ip = false,
    .eth_ip = "0.0.0.0",
#endif
    .wifi_ap_enabled = ENABLE_WIFI_AP,
    .wifi_ap_ip = "0.0.0.0",

    .wifi_sta_enabled = ENABLE_WIFI_STA,
    .wifi_sta_connected = false,
    .wifi_sta_ip = "0.0.0.0",
};

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    snprintf(dst, dst_len, "%s", src);
}

static size_t bounded_strlen(const char *s, size_t max_len)
{
    const char *end = memchr(s, '\0', max_len);
    return end ? (size_t)(end - s) : max_len;
}

static void net_settings_set_defaults(net_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));

    settings->wifi_ap_enabled = ENABLE_WIFI_AP;
    copy_string(settings->wifi_ap_ssid, sizeof(settings->wifi_ap_ssid), WIFI_AP_SSID);
    copy_string(settings->wifi_ap_password, sizeof(settings->wifi_ap_password), WIFI_AP_PASS);

    settings->wifi_sta_enabled = ENABLE_WIFI_STA;
    copy_string(settings->wifi_sta_ssid, sizeof(settings->wifi_sta_ssid), WIFI_STA_SSID);
    copy_string(settings->wifi_sta_password, sizeof(settings->wifi_sta_password), WIFI_STA_PASS);

    copy_string(settings->hostname, sizeof(settings->hostname), NET_DEFAULT_HOSTNAME);

    settings->ip_mode = NET_DEFAULT_USE_STATIC_IP ? NET_IP_MODE_STATIC : NET_IP_MODE_DHCP;
    copy_string(settings->static_ip, sizeof(settings->static_ip), NET_DEFAULT_STATIC_IP);
    copy_string(settings->static_netmask, sizeof(settings->static_netmask), NET_DEFAULT_STATIC_NETMASK);
    copy_string(settings->static_gateway, sizeof(settings->static_gateway), NET_DEFAULT_STATIC_GATEWAY);
    copy_string(settings->static_dns, sizeof(settings->static_dns), NET_DEFAULT_STATIC_DNS);
}

static esp_err_t setting_error(char *err_msg, size_t err_msg_len, const char *msg)
{
    if (err_msg && err_msg_len > 0) {
        snprintf(err_msg, err_msg_len, "%s", msg);
    }

    return ESP_ERR_INVALID_ARG;
}

static bool valid_ipv4(const char *value)
{
    esp_ip4_addr_t addr;

    if (!value || value[0] == '\0') {
        return false;
    }

    if (esp_netif_str_to_ip4(value, &addr) != ESP_OK) {
        return false;
    }

    return addr.addr != 0;
}

static bool valid_hostname(const char *hostname)
{
    size_t len = bounded_strlen(hostname, NET_HOSTNAME_MAX_LEN);

    if (len == 0 || len >= NET_HOSTNAME_MAX_LEN) {
        return false;
    }

    if (hostname[0] == '-' || hostname[len - 1] == '-') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)hostname[i];

        if (!isalnum(ch) && ch != '-') {
            return false;
        }
    }

    return true;
}

static bool settings_equal(const net_settings_t *a, const net_settings_t *b)
{
    return a->wifi_ap_enabled == b->wifi_ap_enabled &&
           strcmp(a->wifi_ap_ssid, b->wifi_ap_ssid) == 0 &&
           strcmp(a->wifi_ap_password, b->wifi_ap_password) == 0 &&
           a->wifi_sta_enabled == b->wifi_sta_enabled &&
           strcmp(a->wifi_sta_ssid, b->wifi_sta_ssid) == 0 &&
           strcmp(a->wifi_sta_password, b->wifi_sta_password) == 0 &&
           strcmp(a->hostname, b->hostname) == 0 &&
           a->ip_mode == b->ip_mode &&
           strcmp(a->static_ip, b->static_ip) == 0 &&
           strcmp(a->static_netmask, b->static_netmask) == 0 &&
           strcmp(a->static_gateway, b->static_gateway) == 0 &&
           strcmp(a->static_dns, b->static_dns) == 0;
}

esp_err_t net_manager_validate_settings(const net_settings_t *settings,
                                        char *err_msg,
                                        size_t err_msg_len)
{
    if (!settings) {
        return setting_error(err_msg, err_msg_len, "Missing network settings");
    }

    size_t ap_ssid_len = bounded_strlen(settings->wifi_ap_ssid, sizeof(settings->wifi_ap_ssid));
    size_t ap_pass_len = bounded_strlen(settings->wifi_ap_password, sizeof(settings->wifi_ap_password));
    size_t sta_ssid_len = bounded_strlen(settings->wifi_sta_ssid, sizeof(settings->wifi_sta_ssid));
    size_t sta_pass_len = bounded_strlen(settings->wifi_sta_password, sizeof(settings->wifi_sta_password));

    if (settings->wifi_ap_enabled && (ap_ssid_len == 0 || ap_ssid_len > NET_WIFI_SSID_MAX_LEN)) {
        return setting_error(err_msg, err_msg_len, "Access point SSID must be 1 to 32 characters");
    }

    if (settings->wifi_ap_enabled && ap_pass_len > 0 && ap_pass_len < 8) {
        return setting_error(err_msg, err_msg_len, "Access point password must be blank or at least 8 characters");
    }

    if (ap_pass_len >= NET_WIFI_PASS_MAX_LEN) {
        return setting_error(err_msg, err_msg_len, "Access point password is too long");
    }

    if (settings->wifi_sta_enabled && (sta_ssid_len == 0 || sta_ssid_len > NET_WIFI_SSID_MAX_LEN)) {
        return setting_error(err_msg, err_msg_len, "Wi-Fi station SSID must be 1 to 32 characters");
    }

    if (sta_pass_len >= NET_WIFI_PASS_MAX_LEN) {
        return setting_error(err_msg, err_msg_len, "Wi-Fi station password is too long");
    }

    if (!valid_hostname(settings->hostname)) {
        return setting_error(err_msg, err_msg_len, "Hostname must use letters, numbers, and hyphens only");
    }

    if (settings->ip_mode != NET_IP_MODE_DHCP && settings->ip_mode != NET_IP_MODE_STATIC) {
        return setting_error(err_msg, err_msg_len, "IP address mode is invalid");
    }

    if (settings->ip_mode == NET_IP_MODE_STATIC) {
        if (!valid_ipv4(settings->static_ip)) {
            return setting_error(err_msg, err_msg_len, "Static IP address is invalid");
        }

        if (!valid_ipv4(settings->static_netmask)) {
            return setting_error(err_msg, err_msg_len, "Static netmask is invalid");
        }

        if (!valid_ipv4(settings->static_gateway)) {
            return setting_error(err_msg, err_msg_len, "Static gateway is invalid");
        }

        if (!valid_ipv4(settings->static_dns)) {
            return setting_error(err_msg, err_msg_len, "Static DNS server is invalid");
        }
    }

    if (err_msg && err_msg_len > 0) {
        err_msg[0] = '\0';
    }

    return ESP_OK;
}

static void nvs_read_string(nvs_handle_t nvs, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t ret = nvs_get_str(nvs, key, dst, &len);

    if (ret != ESP_OK) {
        dst[dst_len - 1] = '\0';
    }
}

static esp_err_t net_settings_load(net_settings_t *settings)
{
    net_settings_set_defaults(settings);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t value = 0;

    if (nvs_get_u8(nvs, "ap_en", &value) == ESP_OK) {
        settings->wifi_ap_enabled = value != 0;
    }

    nvs_read_string(nvs, "ap_ssid", settings->wifi_ap_ssid, sizeof(settings->wifi_ap_ssid));
    nvs_read_string(nvs, "ap_pass", settings->wifi_ap_password, sizeof(settings->wifi_ap_password));

    if (nvs_get_u8(nvs, "sta_en", &value) == ESP_OK) {
        settings->wifi_sta_enabled = value != 0;
    }

    nvs_read_string(nvs, "sta_ssid", settings->wifi_sta_ssid, sizeof(settings->wifi_sta_ssid));
    nvs_read_string(nvs, "sta_pass", settings->wifi_sta_password, sizeof(settings->wifi_sta_password));

    nvs_read_string(nvs, "host", settings->hostname, sizeof(settings->hostname));

    if (nvs_get_u8(nvs, "ip_mode", &value) == ESP_OK) {
        settings->ip_mode = value == NET_IP_MODE_STATIC ? NET_IP_MODE_STATIC : NET_IP_MODE_DHCP;
    }

    nvs_read_string(nvs, "ip", settings->static_ip, sizeof(settings->static_ip));
    nvs_read_string(nvs, "mask", settings->static_netmask, sizeof(settings->static_netmask));
    nvs_read_string(nvs, "gw", settings->static_gateway, sizeof(settings->static_gateway));
    nvs_read_string(nvs, "dns", settings->static_dns, sizeof(settings->static_dns));

    nvs_close(nvs);

    char err[128];
    ret = net_manager_validate_settings(settings, err, sizeof(err));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Stored network settings are invalid (%s); using defaults", err);
        net_settings_set_defaults(settings);
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t nvs_set_string_checked(nvs_handle_t nvs, const char *key, const char *value)
{
    return nvs_set_str(nvs, key, value ? value : "");
}

esp_err_t net_manager_save_settings(const net_settings_t *settings)
{
    char err[128];
    esp_err_t ret = net_manager_validate_settings(settings, err, sizeof(err));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Rejected network settings: %s", err);
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, "ap_en", settings->wifi_ap_enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "ap_ssid", settings->wifi_ap_ssid);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "ap_pass", settings->wifi_ap_password);
    if (ret == ESP_OK) ret = nvs_set_u8(nvs, "sta_en", settings->wifi_sta_enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "sta_ssid", settings->wifi_sta_ssid);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "sta_pass", settings->wifi_sta_password);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "host", settings->hostname);
    if (ret == ESP_OK) ret = nvs_set_u8(nvs, "ip_mode", (uint8_t)settings->ip_mode);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "ip", settings->static_ip);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "mask", settings->static_netmask);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "gw", settings->static_gateway);
    if (ret == ESP_OK) ret = nvs_set_string_checked(nvs, "dns", settings->static_dns);
    if (ret == ESP_OK) ret = nvs_commit(nvs);

    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_settings = *settings;
        s_settings_loaded = true;
        ESP_LOGI(TAG, "Saved network settings; reboot required to apply");
    }

    return ret;
}

esp_err_t net_manager_restore_defaults(void)
{
    net_settings_t defaults;

    net_settings_set_defaults(&defaults);
    return net_manager_save_settings(&defaults);
}

void net_manager_get_settings(net_settings_t *settings)
{
    if (!settings) {
        return;
    }

    if (!s_settings_loaded) {
        net_settings_set_defaults(&s_settings);
        s_active_settings = s_settings;
        s_settings_loaded = true;
    }

    *settings = s_settings;
}

bool net_manager_settings_pending_reboot(void)
{
    if (!s_settings_loaded) {
        return false;
    }

    return !settings_equal(&s_settings, &s_active_settings);
}

static void ip_to_str(esp_netif_t *netif, char out[16])
{
    esp_netif_ip_info_t ip;

    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(out, 16, IPSTR, IP2STR(&ip.ip));
    } else {
        copy_string(out, 16, "0.0.0.0");
    }
}

void net_manager_get_status(net_status_t *status)
{
    if (!status) {
        return;
    }

    *status = s_status;
}

static esp_err_t configure_lan_netif(esp_netif_t *netif, const char *name)
{
    if (!netif) {
        return ESP_OK;
    }

    esp_err_t ret = esp_netif_set_hostname(netif, s_active_settings.hostname);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s hostname set failed: %s", name, esp_err_to_name(ret));
    }

    if (s_active_settings.ip_mode != NET_IP_MODE_STATIC) {
        return ESP_OK;
    }

    ret = esp_netif_dhcpc_stop(netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "%s DHCP stop failed: %s", name, esp_err_to_name(ret));
        return ret;
    }

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));

    if (esp_netif_str_to_ip4(s_active_settings.static_ip, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(s_active_settings.static_netmask, &ip_info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(s_active_settings.static_gateway, &ip_info.gw) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_netif_set_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s static IP set failed: %s", name, esp_err_to_name(ret));
        return ret;
    }

    esp_netif_dns_info_t dns_info;
    memset(&dns_info, 0, sizeof(dns_info));
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;

    if (esp_netif_str_to_ip4(s_active_settings.static_dns, &dns_info.ip.u_addr.ip4) == ESP_OK) {
        ret = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s DNS set failed: %s", name, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "%s static IPv4: %s/%s gw %s dns %s",
             name,
             s_active_settings.static_ip,
             s_active_settings.static_netmask,
             s_active_settings.static_gateway,
             s_active_settings.static_dns);

    return ESP_OK;
}

#if ENABLE_CAPTIVE_PORTAL
static uint16_t dns_get_u16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static void dns_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void dns_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static int dns_find_question_end(const uint8_t *packet, int len, int pos)
{
    while (pos < len) {
        uint8_t label_len = packet[pos++];

        if (label_len == 0) {
            return pos;
        }

        if ((label_len & 0xC0) != 0 || pos + label_len > len) {
            return -1;
        }

        pos += label_len;
    }

    return -1;
}

static int captive_dns_build_response(const uint8_t *query,
                                      int query_len,
                                      uint8_t *response,
                                      size_t response_len)
{
    if (query_len < 12 || response_len < (size_t)query_len + 16) {
        return -1;
    }

    uint16_t qdcount = dns_get_u16(query + 4);
    if (qdcount == 0) {
        return -1;
    }

    int qname_end = dns_find_question_end(query, query_len, 12);
    if (qname_end < 0 || qname_end + 4 > query_len) {
        return -1;
    }

    uint16_t qtype = dns_get_u16(query + qname_end);
    uint16_t qclass = dns_get_u16(query + qname_end + 2);
    bool answer_ipv4 = (qtype == 1 || qtype == 255) && (qclass == 1 || qclass == 255);

    int question_len = qname_end + 4;
    size_t out_len = (size_t)question_len + (answer_ipv4 ? 16u : 0u);
    if (out_len > response_len) {
        return -1;
    }

    memcpy(response, query, (size_t)question_len);

    response[2] = 0x81;
    response[3] = 0x80;
    dns_put_u16(response + 4, 1);
    dns_put_u16(response + 6, answer_ipv4 ? 1 : 0);
    dns_put_u16(response + 8, 0);
    dns_put_u16(response + 10, 0);

    if (!answer_ipv4) {
        return question_len;
    }

    uint8_t *answer = response + question_len;
    answer[0] = 0xC0;
    answer[1] = 0x0C;
    dns_put_u16(answer + 2, 1);
    dns_put_u16(answer + 4, 1);
    dns_put_u32(answer + 6, CAPTIVE_PORTAL_DNS_TTL_SECONDS);
    dns_put_u16(answer + 10, 4);
    memcpy(answer + 12, &s_captive_dns_ip.addr, 4);

    return (int)out_len;
}

static void captive_dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Captive portal DNS socket create failed");
        s_captive_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CAPTIVE_PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGW(TAG, "Captive portal DNS bind failed on port %d", CAPTIVE_PORTAL_DNS_PORT);
        close(sock);
        s_captive_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive portal DNS started on port %d", CAPTIVE_PORTAL_DNS_PORT);

    uint8_t query[512];
    uint8_t response[560];

    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, query, sizeof(query), 0,
                           (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }

        int response_len = captive_dns_build_response(query, len, response, sizeof(response));
        if (response_len <= 0) {
            continue;
        }

        sendto(sock, response, (size_t)response_len, 0,
               (struct sockaddr *)&from, from_len);
    }
}

static void captive_dns_start(void)
{
    if (s_captive_dns_task != NULL || s_ap_netif == NULL) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "Captive portal DNS could not read AP IP");
        return;
    }

    s_captive_dns_ip = ip_info.ip;

    BaseType_t ok = xTaskCreate(
        captive_dns_task,
        "portal_dns",
        CAPTIVE_PORTAL_DNS_TASK_STACK,
        NULL,
        CAPTIVE_PORTAL_DNS_TASK_PRIORITY,
        &s_captive_dns_task
    );

    if (ok != pdPASS) {
        s_captive_dns_task = NULL;
        ESP_LOGW(TAG, "Failed to start captive portal DNS task");
    }
}

static void configure_ap_captive_portal_options(void)
{
    if (s_ap_netif == NULL) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read AP IP for captive portal options");
        return;
    }

    esp_netif_dns_info_t dns_info;
    memset(&dns_info, 0, sizeof(dns_info));
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4 = ip_info.ip;

    esp_err_t ret = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AP DNS option address failed: %s", esp_err_to_name(ret));
    }

    uint8_t offer_dns = 1;
    ret = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns,
                                 sizeof(offer_dns));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AP DHCP DNS offer failed: %s", esp_err_to_name(ret));
    }

    int uri_len = snprintf(s_captive_portal_uri,
                           sizeof(s_captive_portal_uri),
                           "http://%s.local/",
                           s_active_settings.hostname);
    const char *portal_uri = (uri_len > 0 && (size_t)uri_len < sizeof(s_captive_portal_uri))
                                ? s_captive_portal_uri
                                : CAPTIVE_PORTAL_URI;

    ret = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)portal_uri,
                                 strlen(portal_uri));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AP DHCP captive portal URI failed: %s", esp_err_to_name(ret));
    }
}
#endif

#if ENABLE_MDNS
static esp_err_t mdns_start_services(void)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_hostname_set(s_active_settings.hostname);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mdns_instance_name_set(MDNS_INSTANCE_NAME);

    ret = mdns_service_add(MDNS_INSTANCE_NAME,
                           MDNS_HTTP_SERVICE_NAME,
                           MDNS_HTTP_SERVICE_PROTO,
                           MDNS_HTTP_SERVICE_PORT,
                           NULL,
                           0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS HTTP service add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "mDNS started: http://%s.local/", s_active_settings.hostname);
    return ESP_OK;
}
#endif

#if BOARD_VERSION == 2

static void w5500_reset(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << ETH_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_LOGI(TAG, "Resetting W5500");

    gpio_set_level(ETH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(ETH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "ETH started");
            break;

        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ETH link up");
            s_status.eth_link = true;
            if (s_active_settings.ip_mode == NET_IP_MODE_STATIC) {
                s_status.eth_has_ip = true;
                ip_to_str(s_eth_netif, s_status.eth_ip);
                ESP_LOGI(TAG, "ETH static IP: %s", s_status.eth_ip);
            }
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "ETH link down");
            s_status.eth_link = false;
            s_status.eth_has_ip = false;
            copy_string(s_status.eth_ip, sizeof(s_status.eth_ip), "0.0.0.0");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "ETH stopped");
            s_status.eth_link = false;
            s_status.eth_has_ip = false;
            copy_string(s_status.eth_ip, sizeof(s_status.eth_ip), "0.0.0.0");
            break;

        default:
            break;
    }
}

#endif /* BOARD_VERSION == 2 - end of eth_event_handler */

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
#if BOARD_VERSION == 2
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        s_status.eth_has_ip = true;
        ip_to_str(s_eth_netif, s_status.eth_ip);

        ESP_LOGI(TAG, "ETH got IP: %s", s_status.eth_ip);
    }
#endif

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_status.wifi_sta_connected = true;
        ip_to_str(s_sta_netif, s_status.wifi_sta_ip);

        ESP_LOGI(TAG, "Wi-Fi STA got IP: %s", s_status.wifi_sta_ip);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA start");
            if (s_active_settings.wifi_sta_enabled) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            if (s_active_settings.ip_mode == NET_IP_MODE_STATIC) {
                s_status.wifi_sta_connected = true;
                ip_to_str(s_sta_netif, s_status.wifi_sta_ip);
                ESP_LOGI(TAG, "Wi-Fi STA static IP: %s", s_status.wifi_sta_ip);
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi STA disconnected");
            s_status.wifi_sta_connected = false;
            copy_string(s_status.wifi_sta_ip, sizeof(s_status.wifi_sta_ip), "0.0.0.0");
            if (s_active_settings.wifi_sta_enabled) {
                esp_wifi_connect();
            }
            break;

        default:
            break;
    }
}

#if BOARD_VERSION == 2

static esp_err_t eth_start_w5500(void)
{
    ESP_LOGI(TAG, "Starting W5500 Ethernet");

    w5500_reset();

    spi_bus_config_t buscfg = {
        .mosi_io_num = ETH_MOSI_GPIO,
        .miso_io_num = ETH_MISO_GPIO,
        .sclk_io_num = ETH_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 60 * 1000 * 1000,
        .spics_io_num = ETH_CS_GPIO,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);

    w5500_config.int_gpio_num = ETH_INT_GPIO;

#if ETH_INT_GPIO < 0
    w5500_config.poll_period_ms = 100;
#endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth_handle));

    uint8_t eth_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    ESP_LOGI(TAG, "ETH MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             eth_mac[0], eth_mac[1], eth_mac[2],
             eth_mac[3], eth_mac[4], eth_mac[5]);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));
    ESP_ERROR_CHECK(configure_lan_netif(s_eth_netif, "ETH"));

    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT,
        ESP_EVENT_ANY_ID,
        eth_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_ETH_GOT_IP,
        ip_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    return ESP_OK;
}

#endif /* BOARD_VERSION == 2 - end of eth_start_w5500 */

static esp_err_t wifi_start(void)
{
    bool ap_enabled = s_active_settings.wifi_ap_enabled;
    bool sta_enabled = s_active_settings.wifi_sta_enabled;

    s_status.wifi_ap_enabled = false;
    s_status.wifi_sta_enabled = false;
    s_status.wifi_sta_connected = false;
    copy_string(s_status.wifi_ap_ip, sizeof(s_status.wifi_ap_ip), "0.0.0.0");
    copy_string(s_status.wifi_sta_ip, sizeof(s_status.wifi_sta_ip), "0.0.0.0");

    if (!ap_enabled && !sta_enabled) {
        ESP_LOGI(TAG, "Wi-Fi disabled by settings");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Wi-Fi (%s%s%s)",
             ap_enabled ? "AP" : "",
             ap_enabled && sta_enabled ? "+" : "",
             sta_enabled ? "STA" : "");

    if (ap_enabled) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            return ESP_FAIL;
        }

#if ENABLE_CAPTIVE_PORTAL
        configure_ap_captive_portal_options();
#endif
    }

    if (sta_enabled) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            return ESP_FAIL;
        }

        ESP_ERROR_CHECK(configure_lan_netif(s_sta_netif, "Wi-Fi STA"));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (sta_enabled) {
        ESP_ERROR_CHECK(esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        ));

        ESP_ERROR_CHECK(esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            ip_event_handler,
            NULL
        ));
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (ap_enabled && sta_enabled) {
        mode = WIFI_MODE_APSTA;
    } else if (ap_enabled) {
        mode = WIFI_MODE_AP;
    } else {
        mode = WIFI_MODE_STA;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (ap_enabled) {
        wifi_config_t ap_config = {0};
        copy_string((char *)ap_config.ap.ssid,
                    sizeof(ap_config.ap.ssid),
                    s_active_settings.wifi_ap_ssid);
        copy_string((char *)ap_config.ap.password,
                    sizeof(ap_config.ap.password),
                    s_active_settings.wifi_ap_password);
        ap_config.ap.ssid_len = strlen(s_active_settings.wifi_ap_ssid);
        ap_config.ap.channel = WIFI_AP_CHANNEL;
        ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
        ap_config.ap.authmode = strlen(s_active_settings.wifi_ap_password) == 0
                                    ? WIFI_AUTH_OPEN
                                    : WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    if (sta_enabled) {
        wifi_config_t sta_config = {0};
        copy_string((char *)sta_config.sta.ssid,
                    sizeof(sta_config.sta.ssid),
                    s_active_settings.wifi_sta_ssid);
        copy_string((char *)sta_config.sta.password,
                    sizeof(sta_config.sta.password),
                    s_active_settings.wifi_sta_password);

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (sta_enabled) {
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    if (ap_enabled) {
        s_status.wifi_ap_enabled = true;
        ip_to_str(s_ap_netif, s_status.wifi_ap_ip);

        ESP_LOGI(TAG, "Wi-Fi AP SSID: %s", s_active_settings.wifi_ap_ssid);
        ESP_LOGI(TAG, "Wi-Fi AP IP: %s", s_status.wifi_ap_ip);

#if ENABLE_CAPTIVE_PORTAL
        captive_dns_start();
#endif
    }

    if (sta_enabled) {
        s_status.wifi_sta_enabled = true;
        ESP_LOGI(TAG, "Wi-Fi STA SSID: %s", s_active_settings.wifi_sta_ssid);
    }

    return ESP_OK;
}

esp_err_t net_manager_start(void)
{
    ESP_ERROR_CHECK(net_settings_load(&s_settings));
    s_active_settings = s_settings;
    s_settings_loaded = true;

    ESP_LOGI(TAG, "Network hostname: %s", s_active_settings.hostname);
    ESP_LOGI(TAG, "LAN IP mode: %s",
             s_active_settings.ip_mode == NET_IP_MODE_STATIC ? "static" : "dhcp");

#if BOARD_VERSION == 2
    ESP_ERROR_CHECK(eth_start_w5500());
#endif
    ESP_ERROR_CHECK(wifi_start());

#if ENABLE_MDNS
    (void)mdns_start_services();
#endif

    return ESP_OK;
}
