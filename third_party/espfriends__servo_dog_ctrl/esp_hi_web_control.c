/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_hi_web_control.h"
#include "servo_control_setting.h"
#include "servo_dog_ctrl.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_ESP32C3
#include "mdns.h"
#endif

#define TAG "HTTP_SERVER"
#define WEB_SEARCH_NVS_NAMESPACE "web_search"
#define BRAVE_API_KEY_NVS_KEY "brave_api_key"
#define BRAVE_LLM_CONTEXT_API_KEY_NVS_KEY "llm_ctx_key"
#define BRAVE_USE_LLM_CONTEXT_NVS_KEY "use_llm_ctx"

static bool is_calibration_mode = false;
static httpd_handle_t server_handle = NULL;

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");
extern const uint8_t calibration_png_start[] asm("_binary_calibration_png_start");
extern const uint8_t calibration_png_end[] asm("_binary_calibration_png_end");
extern const uint8_t nipplejs_min_js_start[] asm("_binary_nipplejs_min_js_start");
extern const uint8_t nipplejs_min_js_end[] asm("_binary_nipplejs_min_js_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t main_js_start[] asm("_binary_main_js_start");
extern const uint8_t main_js_end[] asm("_binary_main_js_end");

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".js")) {
        return httpd_resp_set_type(req, "text/javascript");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    const uint8_t *file_start = NULL;
    const uint8_t *file_end = NULL;
    size_t file_size = 0;

    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    if (strcmp(uri, "/index.html") == 0) {
        file_start = index_html_start;
        file_end = index_html_end;
    } else if (strcmp(uri, "/favicon.ico") == 0) {
        file_start = favicon_ico_start;
        file_end = favicon_ico_end;
    } else if (strcmp(uri, "/calibration.png") == 0) {
        file_start = calibration_png_start;
        file_end = calibration_png_end;
    } else if (strcmp(uri, "/nipplejs.min.js") == 0) {
        file_start = nipplejs_min_js_start;
        file_end = nipplejs_min_js_end;
    } else if (strcmp(uri, "/styles.css") == 0) {
        file_start = styles_css_start;
        file_end = styles_css_end;
    } else if (strcmp(uri, "/main.js") == 0) {
        file_start = main_js_start;
        file_end = main_js_end;
    } else {
        ESP_LOGE(TAG, "File not found: %s", uri);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    file_size = file_end - file_start;
    set_content_type_from_file(req, uri);

    httpd_resp_send(req, (const char *)file_start, file_size);
    return ESP_OK;
}

// API: POST /reset
static esp_err_t start_calibration_handler_func(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Start calibration request received");
    char response[128];
    int fl = 0, fr = 0, bl = 0, br = 0;
    servo_control_get_save_value(&fl, &fr, &bl, &br);
    ESP_LOGI(TAG, "FL: %d, FR: %d, BL: %d, BR: %d", fl, fr, bl, br);
    int len = snprintf(response, sizeof(response), "{\"fl\":%d,\"fr\":%d,\"bl\":%d,\"br\":%d}", fl, fr, bl, br);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, len);
    servo_dog_ctrl_send(DOG_STATE_INSTALLATION, NULL);
    is_calibration_mode = true;
    return ESP_OK;
}

static esp_err_t exit_calibration_handler_func(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Exit calibration request received");
    servo_dog_ctrl_send(DOG_STATE_IDLE, NULL);
    is_calibration_mode = false;
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t adjust_handler_func(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Adjust request received");
    char content[128] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"empty request\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Content: %s", content);
    char leg_str[16] = {0};
    int leg_value = 0;

    int fl = 0, fr = 0, bl = 0, br = 0;
    servo_control_get_save_value(&fl, &fr, &bl, &br);
    // {"servo":"bl","value":1}
    sscanf(content, "{\"servo\":\"%[^\"]\",\"value\":%d}", leg_str, &leg_value);
    if (strcmp(leg_str, "fl") == 0) {
        ESP_LOGI(TAG, "Front left: %d", leg_value);
        fl = leg_value;
    } else if (strcmp(leg_str, "fr") == 0) {
        ESP_LOGI(TAG, "Front right: %d", leg_value);
        fr = leg_value;
    } else if (strcmp(leg_str, "bl") == 0) {
        ESP_LOGI(TAG, "Back left: %d", leg_value);
        bl = leg_value;
    } else if (strcmp(leg_str, "br") == 0) {
        ESP_LOGI(TAG, "Back right: %d", leg_value);
        br = leg_value;
    }

    servo_control_set_save_value(fl, fr, bl, br);
    servo_dog_ctrl_send(DOG_STATE_INSTALLATION, NULL);
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// API: POST /control
static esp_err_t control_handler_func(httpd_req_t *req)
{
    if (is_calibration_mode) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\": \"Control disabled in calibration mode\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Control request received");
    char content[128] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"empty request\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Content: %s", content);

    char function[16];
    char value[16];
    // {"%s":"%s"}
    sscanf(content, "{\"%[^\"]\":\"%[^\"]\"}", function, value);
    if (strcmp(function, "move") == 0) {
        ESP_LOGI(TAG, "Move request received");
        switch (value[0]) {
            case 'F':
                ESP_LOGI(TAG, "Forward");
                servo_dog_ctrl_send(DOG_STATE_FORWARD, NULL);
                break;
            case 'B':
                ESP_LOGI(TAG, "Backward");
                servo_dog_ctrl_send(DOG_STATE_BACKWARD, NULL);
                break;
            case 'L':
                ESP_LOGI(TAG, "Left");
                servo_dog_ctrl_send(DOG_STATE_TURN_LEFT, NULL);
                break;
            case 'R':
                ESP_LOGI(TAG, "Right");
                servo_dog_ctrl_send(DOG_STATE_TURN_RIGHT, NULL);
                break;
        }
    } else if (strcmp(function, "action") == 0) {
        ESP_LOGI(TAG, "Action request received %s", value);
        servo_dog_ctrl_send(atoi(value) + DOG_STATE_TURN_LEFT, NULL);
    }
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t read_request_body(httpd_req_t *req, char *content, size_t content_size)
{
    if (req->content_len >= content_size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"payload too large\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, content + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"failed to read request body\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        received += ret;
    }
    content[received] = '\0';
    return ESP_OK;
}

static bool extract_string_from_json(const char *content, const char *field, char *value, size_t value_size)
{
    char key_pattern[64];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", field);
    const char *key = strstr(content, key_pattern);
    if (key == NULL) {
        return false;
    }

    const char *colon = strchr(key, ':');
    if (colon == NULL) {
        return false;
    }

    const char *value_start = strchr(colon, '"');
    if (value_start == NULL) {
        return false;
    }
    value_start++;

    const char *value_end = strchr(value_start, '"');
    if (value_end == NULL || value_end < value_start) {
        return false;
    }

    size_t len = value_end - value_start;
    if (len >= value_size) {
        return false;
    }

    memcpy(value, value_start, len);
    value[len] = '\0';
    return true;
}

static bool extract_bool_from_json(const char *content, const char *field, bool *value)
{
    char key_pattern[64];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", field);
    const char *key = strstr(content, key_pattern);
    if (key == NULL) {
        return false;
    }

    const char *colon = strchr(key, ':');
    if (colon == NULL) {
        return false;
    }

    const char *value_start = colon + 1;
    while (*value_start == ' ' || *value_start == '\t' || *value_start == '\r' || *value_start == '\n') {
        value_start++;
    }

    if (strncmp(value_start, "true", 4) == 0) {
        *value = true;
        return true;
    }
    if (strncmp(value_start, "false", 5) == 0) {
        *value = false;
        return true;
    }

    return false;
}

static bool brave_api_key_is_configured(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEB_SEARCH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t length = 0;
    err = nvs_get_str(handle, BRAVE_API_KEY_NVS_KEY, NULL, &length);
    nvs_close(handle);
    return err == ESP_OK && length > 1;
}

static bool brave_llm_context_api_key_is_configured(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEB_SEARCH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t length = 0;
    err = nvs_get_str(handle, BRAVE_LLM_CONTEXT_API_KEY_NVS_KEY, NULL, &length);
    nvs_close(handle);
    return err == ESP_OK && length > 1;
}

static bool brave_use_llm_context_enabled(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEB_SEARCH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, BRAVE_USE_LLM_CONTEXT_NVS_KEY, &value);
    nvs_close(handle);
    return err == ESP_OK && value != 0;
}

// API: GET /brave_search_config
static esp_err_t brave_search_config_get_handler_func(httpd_req_t *req)
{
    char response[160];
    int len = snprintf(response, sizeof(response),
        "{\"configured\":%s,\"web_configured\":%s,\"llm_context_configured\":%s,\"use_llm_context\":%s}",
        brave_api_key_is_configured() ? "true" : "false",
        brave_api_key_is_configured() ? "true" : "false",
        brave_llm_context_api_key_is_configured() ? "true" : "false",
        brave_use_llm_context_enabled() ? "true" : "false");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

// API: POST /brave_search_config
static esp_err_t brave_search_config_post_handler_func(httpd_req_t *req)
{
    char content[512] = {0};
    char web_api_key[256] = {0};
    char llm_context_api_key[256] = {0};
    bool use_llm_context = false;
    if (read_request_body(req, content, sizeof(content)) != ESP_OK) {
        return ESP_OK;
    }

    bool has_web_api_key = extract_string_from_json(content, "web_api_key", web_api_key, sizeof(web_api_key));
    if (!has_web_api_key) {
        has_web_api_key = extract_string_from_json(content, "api_key", web_api_key, sizeof(web_api_key));
    }
    bool has_llm_context_api_key = extract_string_from_json(content, "llm_context_api_key",
        llm_context_api_key, sizeof(llm_context_api_key));
    bool has_use_llm_context = extract_bool_from_json(content, "use_llm_context", &use_llm_context);
    if (!has_web_api_key && !has_llm_context_api_key && !has_use_llm_context) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"missing settings\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEB_SEARCH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open web search NVS namespace: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"nvs open failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (has_web_api_key) {
        if (web_api_key[0] == '\0') {
            err = nvs_erase_key(handle, BRAVE_API_KEY_NVS_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_str(handle, BRAVE_API_KEY_NVS_KEY, web_api_key);
        }
    }

    if (err == ESP_OK && has_llm_context_api_key) {
        if (llm_context_api_key[0] == '\0') {
            err = nvs_erase_key(handle, BRAVE_LLM_CONTEXT_API_KEY_NVS_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_str(handle, BRAVE_LLM_CONTEXT_API_KEY_NVS_KEY, llm_context_api_key);
        }
    }

    if (err == ESP_OK && has_use_llm_context) {
        err = nvs_set_u8(handle, BRAVE_USE_LLM_CONTEXT_NVS_KEY, use_llm_context ? 1 : 0);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update Brave Search API key: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"nvs write failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (has_web_api_key) {
        ESP_LOGI(TAG, "Brave Web Search API key %s", web_api_key[0] == '\0' ? "cleared" : "updated");
    }
    if (has_llm_context_api_key) {
        ESP_LOGI(TAG, "Brave LLM Context API key %s",
            llm_context_api_key[0] == '\0' ? "cleared" : "updated");
    }
    if (has_use_llm_context) {
        ESP_LOGI(TAG, "Brave LLM Context setting %s", use_llm_context ? "enabled" : "disabled");
    }
    char response[160];
    int len = snprintf(response, sizeof(response),
        "{\"configured\":%s,\"web_configured\":%s,\"llm_context_configured\":%s,\"use_llm_context\":%s}",
        brave_api_key_is_configured() ? "true" : "false",
        brave_api_key_is_configured() ? "true" : "false",
        brave_llm_context_api_key_is_configured() ? "true" : "false",
        brave_use_llm_context_enabled() ? "true" : "false");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}
static esp_err_t start_webserver(void)
{
    if (server_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 1024 * 4;
    config.task_priority = 10;
    config.max_open_sockets = 3;
    config.backlog_conn = 2;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    // HTTP_GET /start_calibration
    httpd_uri_t start_calibration_handler = {
        .uri = "/start_calibration",
        .method = HTTP_GET,
        .handler = start_calibration_handler_func,
        .user_ctx = NULL
    };

    // HTTP_GET /exit_calibration
    httpd_uri_t exit_calibration_handler = {
        .uri = "/exit_calibration",
        .method = HTTP_GET,
        .handler = exit_calibration_handler_func,
        .user_ctx = NULL
    };

    // POST /adjust
    httpd_uri_t adjust_handler = {
        .uri = "/adjust",
        .method = HTTP_POST,
        .handler = adjust_handler_func,
        .user_ctx = NULL
    };

    // Post /control
    httpd_uri_t control_handler = {
        .uri = "/control",
        .method = HTTP_POST,
        .handler = control_handler_func,
        .user_ctx = NULL
    };

    // GET /brave_search_config
    httpd_uri_t brave_search_config_get_handler = {
        .uri = "/brave_search_config",
        .method = HTTP_GET,
        .handler = brave_search_config_get_handler_func,
        .user_ctx = NULL
    };

    // POST /brave_search_config
    httpd_uri_t brave_search_config_post_handler = {
        .uri = "/brave_search_config",
        .method = HTTP_POST,
        .handler = brave_search_config_post_handler_func,
        .user_ctx = NULL
    };

    httpd_uri_t static_handler = {
        .uri = "*",
        .method = HTTP_GET,
        .handler = static_file_handler,
        .user_ctx = NULL
    };

    config.core_id = 0;
    esp_err_t err = httpd_start(&server_handle, &config);
    if (err != ESP_OK) {
        server_handle = NULL;
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(server_handle, &start_calibration_handler);
    httpd_register_uri_handler(server_handle, &exit_calibration_handler);
    httpd_register_uri_handler(server_handle, &adjust_handler);
    httpd_register_uri_handler(server_handle, &control_handler);
    httpd_register_uri_handler(server_handle, &brave_search_config_get_handler);
    httpd_register_uri_handler(server_handle, &brave_search_config_post_handler);
    httpd_register_uri_handler(server_handle, &static_handler);

    return ESP_OK;
}

void start_mdns_service(void) {
#if CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "MDNS disabled on ESP32-C3 to preserve SRAM; use the device IP address");
#else
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS init failed");
        return;
    }
    err = mdns_hostname_set(CONFIG_ESP_HI_MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS hostname set failed");
        return;
    }
    err = mdns_instance_name_set("ESP-Hi Web Control");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS instance name set failed");
        return;
    }
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS service add failed");
        return;
    }
    err = mdns_service_instance_name_set("_http", "_tcp", "ESP-Hi Web Control");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS service instance name set failed");
        return;
    }
    ESP_LOGI(TAG, "MDNS service started");
#endif
}

esp_err_t esp_hi_web_control_server_init(void)
{
    servo_control_init();
    esp_err_t err = start_webserver();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed");
        return err;
    }
    start_mdns_service();
    return ESP_OK;
}

esp_err_t esp_hi_web_control_server_deinit(void)
{
    if (server_handle == NULL) {
        return ESP_OK;
    }

    httpd_handle_t server = server_handle;
    server_handle = NULL;
    return httpd_stop(server);
}
