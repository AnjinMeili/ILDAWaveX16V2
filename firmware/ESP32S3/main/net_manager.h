#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NET_WIFI_SSID_MAX_LEN 32
#define NET_WIFI_PASS_MAX_LEN 64
#define NET_HOSTNAME_MAX_LEN 32
#define NET_IPV4_STR_MAX_LEN 16

typedef enum {
    NET_IP_MODE_DHCP = 0,
    NET_IP_MODE_STATIC = 1,
} net_ip_mode_t;

typedef struct {
    bool wifi_ap_enabled;
    char wifi_ap_ssid[NET_WIFI_SSID_MAX_LEN + 1];
    char wifi_ap_password[NET_WIFI_PASS_MAX_LEN];

    bool wifi_sta_enabled;
    char wifi_sta_ssid[NET_WIFI_SSID_MAX_LEN + 1];
    char wifi_sta_password[NET_WIFI_PASS_MAX_LEN];

    char hostname[NET_HOSTNAME_MAX_LEN];

    net_ip_mode_t ip_mode;
    char static_ip[NET_IPV4_STR_MAX_LEN];
    char static_netmask[NET_IPV4_STR_MAX_LEN];
    char static_gateway[NET_IPV4_STR_MAX_LEN];
    char static_dns[NET_IPV4_STR_MAX_LEN];
} net_settings_t;

typedef struct {
    bool eth_link;
    bool eth_has_ip;
    char eth_ip[16];

    bool wifi_ap_enabled;
    char wifi_ap_ip[16];

    bool wifi_sta_enabled;
    bool wifi_sta_connected;
    char wifi_sta_ip[16];
} net_status_t;

esp_err_t net_manager_start(void);
void net_manager_get_status(net_status_t *status);
void net_manager_get_settings(net_settings_t *settings);
bool net_manager_settings_pending_reboot(void);
esp_err_t net_manager_save_settings(const net_settings_t *settings);
esp_err_t net_manager_restore_defaults(void);
esp_err_t net_manager_validate_settings(const net_settings_t *settings,
                                        char *err_msg,
                                        size_t err_msg_len);
