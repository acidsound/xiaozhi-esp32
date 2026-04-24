#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "codecs/dummy_audio_codec.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/display.h"
#include "display/oled_display.h"
#include "mcp_server.h"
#include "servo_dog_ctrl.h"
#include "settings.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_event.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "sdkconfig.h"

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
#include "esp_hi_web_control.h"
#endif

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
#include "driver/rmt_tx.h"
#include "led_strip.h"
#endif

#define TAG "XIAO_XING_VQ2"

class XiaoXingVq2 : public WifiBoard {
private:
    static constexpr int kBasicMotionRepeatCount = 1;
    static constexpr int kBasicMotionSpeed = 30;
    static constexpr int kBowLeanRepeatCount = 2;
    static constexpr int kSwayMotionRepeatCount = 4;
    static constexpr int kShakeHandRepeatCount = 10;
    static constexpr int kGestureHoldTimeMs = 500;
    static constexpr int kShakeHandHoldTimeMs = 3000;
    static constexpr int kSwayAngleOffset = 20;
    static constexpr int kWebSearchTimeoutMs = 6000;
    static constexpr size_t kWebSearchMaxBodyBytes = 64 * 1024;
    static constexpr const char* kWebSearchSettingsNamespace = "web_search";
    static constexpr const char* kBraveApiKeySetting = "brave_api_key";
    static constexpr const char* kBraveLlmContextApiKeySetting = "llm_ctx_key";
    static constexpr const char* kBraveUseLlmContextSetting = "use_llm_ctx";

    Button boot_button_;
    Button audio_wake_button_;
    Button move_wake_button_;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool dog_motion_enabled_ = false;
    bool web_server_initialized_ = false;

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
    led_strip_handle_t led_strip_ = nullptr;
    gpio_num_t active_led_gpio_ = GPIO_NUM_NC;
    bool led_on_ = false;
    uint8_t led_brightness_ = 100;
    uint8_t led_red_ = 0xFF;
    uint8_t led_green_ = 0xFF;
    uint8_t led_blue_ = 0xFF;
#endif

    static bool IsValidGpio(gpio_num_t gpio) {
        return gpio != GPIO_NUM_NC;
    }

    static std::string UrlEncode(const std::string& value) {
        static const char hex[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 3);

        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded.push_back(static_cast<char>(c));
            } else if (c == ' ') {
                encoded.push_back('+');
            } else {
                encoded.push_back('%');
                encoded.push_back(hex[c >> 4]);
                encoded.push_back(hex[c & 0x0F]);
            }
        }
        return encoded;
    }

    static std::string JsonString(const cJSON* item, const char* key) {
        if (item == nullptr) {
            return "";
        }
        auto value = cJSON_GetObjectItem(item, key);
        if (!cJSON_IsString(value) || value->valuestring == nullptr) {
            return "";
        }
        return value->valuestring;
    }

    static cJSON* MakeWebSearchError(const std::string& code, const std::string& message) {
        auto root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "code", code.c_str());
        cJSON_AddStringToObject(root, "error", message.c_str());
        cJSON_AddStringToObject(root, "fallback_instruction",
            "검색 결과를 가져오지 못했습니다. 사용자에게 실패 이유를 짧게 설명하고, 최신 정보가 필요하면 API 키/네트워크를 확인한 뒤 다시 요청해 달라고 안내하세요. 일반 지식으로 답할 수 있으면 최신 정보가 아닐 수 있음을 밝히고 간단히 답하세요.");
        return root;
    }

    std::string GetBraveApiKey() {
        Settings settings(kWebSearchSettingsNamespace, false);
        return settings.GetString(kBraveApiKeySetting);
    }

    std::string GetBraveLlmContextApiKey() {
        Settings settings(kWebSearchSettingsNamespace, false);
        return settings.GetString(kBraveLlmContextApiKeySetting);
    }

    bool GetUseLlmContextFallback() {
        Settings settings(kWebSearchSettingsNamespace, false);
        return settings.GetBool(kBraveUseLlmContextSetting, false);
    }

    static bool IsSettingStringConfigured(const char* key) {
        nvs_handle_t handle;
        esp_err_t err = nvs_open(kWebSearchSettingsNamespace, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            return false;
        }

        size_t length = 0;
        err = nvs_get_str(handle, key, nullptr, &length);
        nvs_close(handle);
        return err == ESP_OK && length > 1;
    }

    cJSON* GetWebSearchConfigStatus() {
        auto root = cJSON_CreateObject();
        bool web_configured = IsSettingStringConfigured(kBraveApiKeySetting);
        bool llm_context_configured = IsSettingStringConfigured(kBraveLlmContextApiKeySetting);
        cJSON_AddBoolToObject(root, "configured", web_configured);
        cJSON_AddBoolToObject(root, "web_configured", web_configured);
        cJSON_AddBoolToObject(root, "llm_context_configured", llm_context_configured);
        cJSON_AddBoolToObject(root, "use_llm_context", GetUseLlmContextFallback());
        cJSON_AddStringToObject(root, "setup_path", "Settings > Web Search");
        return root;
    }

    cJSON* ReadHttpBodyLimited(Http* http, int status_code, std::string& body) {
        char buffer[1024];
        while (true) {
            int bytes_read = http->Read(buffer, sizeof(buffer));
            if (bytes_read < 0) {
                auto error = MakeWebSearchError("read_timeout", "Brave Search API 응답을 읽는 중 시간이 초과됐습니다.");
                cJSON_AddNumberToObject(error, "status_code", status_code);
                cJSON_AddNumberToObject(error, "timeout_ms", kWebSearchTimeoutMs);
                return error;
            }
            if (bytes_read == 0) {
                return nullptr;
            }

            if (body.size() + bytes_read > kWebSearchMaxBodyBytes) {
                auto error = MakeWebSearchError("response_too_large", "Brave Search API 응답이 너무 큽니다.");
                cJSON_AddNumberToObject(error, "status_code", status_code);
                cJSON_AddNumberToObject(error, "max_body_bytes", kWebSearchMaxBodyBytes);
                return error;
            }
            body.append(buffer, bytes_read);
        }
    }

    static void AddHeaderIfPresent(cJSON* root, Http* http, const char* json_key, const char* header_key) {
        std::string value = http->GetResponseHeader(header_key);
        if (!value.empty()) {
            cJSON_AddStringToObject(root, json_key, value.c_str());
        }
    }

    static void AddRateLimitHeaders(cJSON* root, Http* http) {
        AddHeaderIfPresent(root, http, "rate_limit", "X-RateLimit-Limit");
        AddHeaderIfPresent(root, http, "rate_limit_remaining", "X-RateLimit-Remaining");
        AddHeaderIfPresent(root, http, "rate_limit_reset", "X-RateLimit-Reset");
        AddHeaderIfPresent(root, http, "rate_limit_policy", "X-RateLimit-Policy");

        int monthly_limit = ParseRateLimitWindowValue(http->GetResponseHeader("X-RateLimit-Limit"), 1);
        int monthly_remaining = ParseRateLimitWindowValue(http->GetResponseHeader("X-RateLimit-Remaining"), 1);
        if (monthly_limit > 0 && monthly_remaining >= 0) {
            cJSON_AddNumberToObject(root, "monthly_quota_limit", monthly_limit);
            cJSON_AddNumberToObject(root, "monthly_quota_remaining", monthly_remaining);
            if (monthly_remaining * 100 <= monthly_limit) {
                cJSON_AddBoolToObject(root, "quota_warning", true);
                std::string message = "이번 달 Brave API 잔여량이 "
                    + std::to_string(monthly_remaining) + "/"
                    + std::to_string(monthly_limit) + "회 남았습니다.";
                cJSON_AddStringToObject(root, "quota_warning_message", message.c_str());
            }
        }
    }

    static bool IsSearchOk(cJSON* response) {
        return cJSON_IsTrue(cJSON_GetObjectItem(response, "ok"));
    }

    static int ParseRateLimitWindowValue(const std::string& value, int index) {
        size_t start = 0;
        for (int current = 0; current <= index; ++current) {
            size_t comma = value.find(',', start);
            if (current == index) {
                size_t end = comma == std::string::npos ? value.size() : comma;
                while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
                    start++;
                }
                while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
                    end--;
                }
                if (start >= end) {
                    return -1;
                }
                return static_cast<int>(std::strtol(value.substr(start, end - start).c_str(), nullptr, 10));
            }
            if (comma == std::string::npos) {
                return -1;
            }
            start = comma + 1;
        }
        return -1;
    }

    static bool IsMonthlyQuotaExhausted(cJSON* response) {
        if (JsonString(response, "code") != "rate_limited") {
            return false;
        }

        int monthly_remaining = ParseRateLimitWindowValue(JsonString(response, "rate_limit_remaining"), 1);
        return monthly_remaining == 0;
    }

    static bool ContainsAny(const std::string& text, const char* const* words, size_t word_count) {
        for (size_t i = 0; i < word_count; ++i) {
            if (text.find(words[i]) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static bool ShouldUseLlmContext(const std::string& query, const std::string& mode) {
        if (mode == "context" || mode == "research" || mode == "deep") {
            return true;
        }
        if (mode == "web" || mode == "quick") {
            return false;
        }

        static const char* const kResearchWords[] = {
            "조사", "자세", "근거", "출처", "본문", "비교", "분석", "왜", "요약", "정리",
            "차이", "장단점", "원인", "배경", "팩트체크", "확인"
        };
        return ContainsAny(query, kResearchWords, sizeof(kResearchWords) / sizeof(kResearchWords[0]));
    }

    static std::string ExtractBraveErrorCode(const std::string& body) {
        cJSON* response = cJSON_Parse(body.c_str());
        if (response == nullptr) {
            return "";
        }

        cJSON* error = cJSON_GetObjectItem(response, "error");
        std::string code = JsonString(error, "code");
        cJSON_Delete(response);
        return code;
    }

    cJSON* SearchBraveWeb(const std::string& query, int max_results, const std::string& api_key) {
        if (query.empty()) {
            return MakeWebSearchError("empty_query", "검색어가 비어 있습니다.");
        }

        if (api_key.empty()) {
            auto error = MakeWebSearchError("missing_api_key", "Brave Web Search API 키가 설정되어 있지 않습니다. 현재 기기의 웹 설정 화면에 접속해 Settings > Web Search에서 Web Search API 키를 저장해 달라고 안내하세요.");
            auto board_info = cJSON_Parse(GetBoardJson().c_str());
            if (board_info != nullptr) {
                auto ip = cJSON_GetObjectItem(board_info, "ip");
                if (cJSON_IsString(ip) && ip->valuestring != nullptr && ip->valuestring[0] != '\0') {
                    std::string setup_url = "http://" + std::string(ip->valuestring) + "/";
                    cJSON_AddStringToObject(error, "setup_url", setup_url.c_str());
                }
                cJSON_Delete(board_info);
            }
            cJSON_AddStringToObject(error, "setup_path", "Settings > Web Search");
            return error;
        }

        max_results = std::clamp(max_results, 1, 3);
        std::string url = "https://api.search.brave.com/res/v1/web/search?q=" + UrlEncode(query)
            + "&count=" + std::to_string(max_results)
            + "&country=kr&search_lang=ko&spellcheck=1&result_filter=web&text_decorations=false&extra_snippets=false";

        auto http = GetNetwork()->CreateHttp(3);
        http->SetTimeout(kWebSearchTimeoutMs);
        http->SetHeader("Accept", "application/json");
        http->SetHeader("Accept-Encoding", "identity");
        http->SetHeader("X-Subscription-Token", api_key);

        ESP_LOGI(TAG, "Brave search: query=%s count=%d", query.c_str(), max_results);
        if (!http->Open("GET", url)) {
            int last_error = http->GetLastError();
            http->Close();
            auto error = MakeWebSearchError("connection_failed", "Brave Search API 연결에 실패했습니다: " + std::to_string(last_error));
            cJSON_AddNumberToObject(error, "timeout_ms", kWebSearchTimeoutMs);
            return error;
        }

        int status_code = http->GetStatusCode();
        if (status_code < 0) {
            http->Close();
            auto error = MakeWebSearchError("header_timeout", "Brave Search API 응답 헤더를 받지 못했습니다.");
            cJSON_AddNumberToObject(error, "timeout_ms", kWebSearchTimeoutMs);
            return error;
        }

        ESP_LOGI(TAG, "Brave search status=%d content-length=%s transfer-encoding=%s rate-remaining=%s",
            status_code,
            http->GetResponseHeader("Content-Length").c_str(),
            http->GetResponseHeader("Transfer-Encoding").c_str(),
            http->GetResponseHeader("X-RateLimit-Remaining").c_str());

        std::string body;
        auto read_error = ReadHttpBodyLimited(http.get(), status_code, body);
        if (read_error != nullptr) {
            AddRateLimitHeaders(read_error, http.get());
        }
        http->Close();
        if (read_error != nullptr) {
            return read_error;
        }
        ESP_LOGI(TAG, "Brave search response: status=%d body=%u bytes",
            status_code, static_cast<unsigned>(body.size()));

        if (status_code != 200) {
            std::string brave_code = ExtractBraveErrorCode(body);
            std::string code = status_code == 401 || status_code == 403 ? "auth_failed" :
                (status_code == 429 ? "rate_limited" : "http_error");
            auto root = MakeWebSearchError(code, "Brave Search API 오류: HTTP " + std::to_string(status_code));
            cJSON_AddNumberToObject(root, "status_code", status_code);
            if (!brave_code.empty()) {
                cJSON_AddStringToObject(root, "brave_error_code", brave_code.c_str());
            }
            AddRateLimitHeaders(root, http.get());
            return root;
        }

        cJSON* response = cJSON_Parse(body.c_str());
        if (response == nullptr) {
            return MakeWebSearchError("invalid_response", "Brave Search API 응답을 JSON으로 해석할 수 없습니다.");
        }

        cJSON* web = cJSON_GetObjectItem(response, "web");
        cJSON* results = web ? cJSON_GetObjectItem(web, "results") : nullptr;
        if (!cJSON_IsArray(results)) {
            cJSON_Delete(response);
            return MakeWebSearchError("no_results", "검색 결과가 없습니다.");
        }

        auto root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "query", query.c_str());
        cJSON_AddStringToObject(root, "provider", "brave");
        AddRateLimitHeaders(root, http.get());

        auto result_array = cJSON_CreateArray();
        std::string summary_hint;
        int added = 0;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, results) {
            if (added >= max_results) {
                break;
            }

            std::string title = JsonString(item, "title");
            std::string url_value = JsonString(item, "url");
            std::string description = JsonString(item, "description");
            if (title.empty() || url_value.empty()) {
                continue;
            }

            auto result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "title", title.c_str());
            cJSON_AddStringToObject(result, "url", url_value.c_str());
            if (!description.empty()) {
                cJSON_AddStringToObject(result, "description", description.c_str());
                if (summary_hint.size() < 900) {
                    summary_hint += title + ": " + description + "\n";
                }
            }
            cJSON_AddItemToArray(result_array, result);
            added++;
        }

        if (added == 0) {
            cJSON_Delete(result_array);
            cJSON_Delete(response);
            return MakeWebSearchError("no_results", "검색 결과가 없습니다.");
        }

        cJSON_AddNumberToObject(root, "count", added);
        cJSON_AddStringToObject(root, "summary_hint", summary_hint.c_str());
        cJSON_AddItemToObject(root, "results", result_array);
        cJSON_Delete(response);
        return root;
    }

    cJSON* SearchBraveLlmContext(const std::string& query, int max_results, const std::string& api_key) {
        if (query.empty()) {
            return MakeWebSearchError("empty_query", "검색어가 비어 있습니다.");
        }

        if (api_key.empty()) {
            auto error = MakeWebSearchError("missing_llm_context_api_key", "Brave LLM Context API 키가 설정되어 있지 않습니다. 현재 기기의 웹 설정 화면에 접속해 Settings > Web Search에서 LLM Context API 키를 저장해 달라고 안내하세요.");
            cJSON_AddStringToObject(error, "setup_path", "Settings > Web Search");
            return error;
        }

        max_results = std::clamp(max_results, 1, 3);
        std::string url = "https://api.search.brave.com/res/v1/llm/context?q=" + UrlEncode(query)
            + "&count=" + std::to_string(max_results)
            + "&maximum_number_of_urls=" + std::to_string(max_results)
            + "&maximum_number_of_tokens=2048&maximum_number_of_snippets=6"
            + "&maximum_number_of_tokens_per_url=1024&maximum_number_of_snippets_per_url=2"
            + "&country=kr&search_lang=ko&context_threshold_mode=strict&enable_local=false";

        auto http = GetNetwork()->CreateHttp(3);
        http->SetTimeout(kWebSearchTimeoutMs);
        http->SetHeader("Accept", "application/json");
        http->SetHeader("Accept-Encoding", "identity");
        http->SetHeader("X-Subscription-Token", api_key);

        ESP_LOGI(TAG, "Brave LLM context: query=%s count=%d", query.c_str(), max_results);
        if (!http->Open("GET", url)) {
            int last_error = http->GetLastError();
            http->Close();
            auto error = MakeWebSearchError("connection_failed", "Brave LLM Context API 연결에 실패했습니다: " + std::to_string(last_error));
            cJSON_AddNumberToObject(error, "timeout_ms", kWebSearchTimeoutMs);
            return error;
        }

        int status_code = http->GetStatusCode();
        if (status_code < 0) {
            http->Close();
            auto error = MakeWebSearchError("header_timeout", "Brave LLM Context API 응답 헤더를 받지 못했습니다.");
            cJSON_AddNumberToObject(error, "timeout_ms", kWebSearchTimeoutMs);
            return error;
        }

        ESP_LOGI(TAG, "Brave LLM context status=%d content-length=%s transfer-encoding=%s rate-remaining=%s",
            status_code,
            http->GetResponseHeader("Content-Length").c_str(),
            http->GetResponseHeader("Transfer-Encoding").c_str(),
            http->GetResponseHeader("X-RateLimit-Remaining").c_str());

        std::string body;
        auto read_error = ReadHttpBodyLimited(http.get(), status_code, body);
        if (read_error != nullptr) {
            AddRateLimitHeaders(read_error, http.get());
        }
        http->Close();
        if (read_error != nullptr) {
            return read_error;
        }
        ESP_LOGI(TAG, "Brave LLM context response: status=%d body=%u bytes",
            status_code, static_cast<unsigned>(body.size()));

        if (status_code != 200) {
            std::string brave_code = ExtractBraveErrorCode(body);
            std::string code = brave_code == "OPTION_NOT_IN_PLAN" ? "llm_context_not_in_plan" :
                (status_code == 401 || status_code == 403 ? "auth_failed" :
                (status_code == 429 ? "rate_limited" : "http_error"));
            auto root = MakeWebSearchError(code, "Brave LLM Context API 오류: HTTP " + std::to_string(status_code));
            cJSON_AddNumberToObject(root, "status_code", status_code);
            if (!brave_code.empty()) {
                cJSON_AddStringToObject(root, "brave_error_code", brave_code.c_str());
            }
            AddRateLimitHeaders(root, http.get());
            return root;
        }

        cJSON* response = cJSON_Parse(body.c_str());
        if (response == nullptr) {
            return MakeWebSearchError("invalid_response", "Brave LLM Context API 응답을 JSON으로 해석할 수 없습니다.");
        }

        cJSON* grounding = cJSON_GetObjectItem(response, "grounding");
        cJSON* results = grounding ? cJSON_GetObjectItem(grounding, "generic") : nullptr;
        if (!cJSON_IsArray(results)) {
            cJSON_Delete(response);
            return MakeWebSearchError("no_results", "검색 컨텍스트 결과가 없습니다.");
        }

        auto root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "query", query.c_str());
        cJSON_AddStringToObject(root, "provider", "brave_llm_context");
        AddRateLimitHeaders(root, http.get());

        auto result_array = cJSON_CreateArray();
        std::string summary_hint;
        int added = 0;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, results) {
            if (added >= max_results) {
                break;
            }

            std::string title = JsonString(item, "title");
            std::string url_value = JsonString(item, "url");
            cJSON* snippets = cJSON_GetObjectItem(item, "snippets");
            if (title.empty() || url_value.empty() || !cJSON_IsArray(snippets)) {
                continue;
            }

            std::string description;
            cJSON* snippet = nullptr;
            cJSON_ArrayForEach(snippet, snippets) {
                if (!cJSON_IsString(snippet) || snippet->valuestring == nullptr) {
                    continue;
                }
                if (!description.empty()) {
                    description += " ";
                }
                description += snippet->valuestring;
                if (description.size() >= 900) {
                    description.resize(900);
                    break;
                }
            }
            if (description.empty()) {
                continue;
            }

            auto result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "title", title.c_str());
            cJSON_AddStringToObject(result, "url", url_value.c_str());
            cJSON_AddStringToObject(result, "description", description.c_str());
            cJSON_AddItemToArray(result_array, result);

            if (summary_hint.size() < 1400) {
                summary_hint += title + ": " + description + "\n";
            }
            added++;
        }

        if (added == 0) {
            cJSON_Delete(result_array);
            cJSON_Delete(response);
            return MakeWebSearchError("no_results", "검색 컨텍스트 결과가 없습니다.");
        }

        cJSON_AddNumberToObject(root, "count", added);
        cJSON_AddStringToObject(root, "summary_hint", summary_hint.c_str());
        cJSON_AddItemToObject(root, "results", result_array);
        cJSON_Delete(response);
        return root;
    }

    cJSON* SearchBrave(const std::string& query, int max_results, const std::string& mode) {
        bool allow_context = GetUseLlmContextFallback();
        bool use_context_first = allow_context && ShouldUseLlmContext(query, mode);
        if (use_context_first) {
            ESP_LOGI(TAG, "Brave search mode=%s, using LLM Context first", mode.c_str());
            auto context_result = SearchBraveLlmContext(query, max_results, GetBraveLlmContextApiKey());
            if (IsSearchOk(context_result)) {
                cJSON_AddStringToObject(context_result, "search_style", "investigation");
                cJSON_AddStringToObject(context_result, "response_hint",
                    "사용자에게 단순히 검색했다고 하지 말고, 관련 출처 본문을 조사해봤다는 식으로 자연스럽게 답하세요.");
                return context_result;
            }

            std::string context_code = JsonString(context_result, "code");
            std::string context_error = JsonString(context_result, "error");
            ESP_LOGW(TAG, "Brave LLM Context failed (%s), falling back to Web Search",
                context_code.c_str());
            auto web_result = SearchBraveWeb(query, max_results, GetBraveApiKey());
            if (IsSearchOk(web_result)) {
                cJSON_AddBoolToObject(web_result, "fallback_from_llm_context", true);
                if (!context_code.empty()) {
                    cJSON_AddStringToObject(web_result, "llm_context_error_code", context_code.c_str());
                }
                if (!context_error.empty()) {
                    cJSON_AddStringToObject(web_result, "llm_context_error", context_error.c_str());
                }
            }
            cJSON_Delete(context_result);
            return web_result;
        }

        auto web_result = SearchBraveWeb(query, max_results, GetBraveApiKey());
        if (IsSearchOk(web_result)) {
            return web_result;
        }

        std::string web_code = JsonString(web_result, "code");
        std::string web_error = JsonString(web_result, "error");
        if (!allow_context || !IsMonthlyQuotaExhausted(web_result)) {
            return web_result;
        }

        ESP_LOGW(TAG, "Brave Web Search monthly quota exhausted, falling back to LLM Context");
        auto context_result = SearchBraveLlmContext(query, max_results, GetBraveLlmContextApiKey());
        if (IsSearchOk(context_result)) {
            cJSON_AddBoolToObject(context_result, "fallback_from_web_search", true);
            if (!web_code.empty()) {
                cJSON_AddStringToObject(context_result, "web_search_error_code", web_code.c_str());
            }
            if (!web_error.empty()) {
                cJSON_AddStringToObject(context_result, "web_search_error", web_error.c_str());
            }
            cJSON_Delete(web_result);
            return context_result;
        }

        std::string context_code = JsonString(context_result, "code");
        std::string context_error = JsonString(context_result, "error");
        if (!context_code.empty()) {
            cJSON_AddStringToObject(web_result, "llm_context_error_code", context_code.c_str());
        }
        if (!context_error.empty()) {
            cJSON_AddStringToObject(web_result, "llm_context_error", context_error.c_str());
        }
        cJSON_Delete(context_result);
        return web_result;
    }

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    static void EnsureServoControlNvsValue(nvs_handle_t handle, const char* key) {
        int32_t value = 0;
        esp_err_t err = nvs_get_i32(handle, key, &value);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(nvs_set_i32(handle, key, 0));
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read servo_control.%s: %s", key, esp_err_to_name(err));
        }
    }

    static void EnsureServoControlNvsDefaults() {
        nvs_handle_t handle;
        esp_err_t err = nvs_open("servo_control", NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to prepare servo_control NVS namespace: %s", esp_err_to_name(err));
            return;
        }

        EnsureServoControlNvsValue(handle, "fl");
        EnsureServoControlNvsValue(handle, "fr");
        EnsureServoControlNvsValue(handle, "bl");
        EnsureServoControlNvsValue(handle, "br");
        ESP_ERROR_CHECK(nvs_commit(handle));
        nvs_close(handle);
    }

    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
            int32_t event_id, void* event_data) {
        (void)event_data;
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
            xTaskCreate(
                [](void* arg) {
                    auto* instance = static_cast<XiaoXingVq2*>(arg);
                    vTaskDelay(pdMS_TO_TICKS(5000));

                    if (!instance->web_server_initialized_) {
                        ESP_LOGI(TAG, "WiFi connected, init web control server");
                        EnsureServoControlNvsDefaults();
                        esp_err_t err = esp_hi_web_control_server_init();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to initialize web control server: %d", err);
                        } else {
                            ESP_LOGI(TAG, "Web control server initialized");
                            instance->web_server_initialized_ = true;
                        }
                    }

                    vTaskDelete(nullptr);
                },
                "web_server_init",
                1024 * 10, arg, 5, nullptr);
        }
    }
#endif

    void HandleMoveWakePressDown(int64_t current_time, int64_t& last_trigger_time, int& gesture_state) {
        int64_t interval = last_trigger_time == 0 ? 0 : current_time - last_trigger_time;
        last_trigger_time = current_time;

        if (interval > 1000) {
            gesture_state = 0;
        } else {
            switch (gesture_state) {
            case 0:
                break;
            case 1:
                if (interval > 300) {
                    gesture_state = 2;
                }
                break;
            case 2:
                if (interval > 100) {
                    gesture_state = 0;
                }
                break;
            }
        }
    }

    void HandleMoveWakePressUp(int64_t current_time, int64_t& last_trigger_time, int& gesture_state) {
        int64_t interval = current_time - last_trigger_time;

        if (interval > 1000) {
            gesture_state = 0;
        } else {
            switch (gesture_state) {
            case 0:
                if (interval > 300) {
                    gesture_state = 1;
                }
                break;
            case 1:
                break;
            case 2:
                if (interval < 100) {
                    ESP_LOGI(TAG, "move wake gesture detected");
                    gesture_state = 0;
                    auto& app = Application::GetInstance();
                    app.ToggleChatState();
                }
                break;
            }
        }
    }

    void InitializeButtons() {
        static int64_t last_trigger_time = 0;
        static int gesture_state = 0;

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        audio_wake_button_.OnPressDown([this]() {
        });

        audio_wake_button_.OnPressUp([this]() {
        });

        move_wake_button_.OnPressDown([this]() {
            int64_t current_time = esp_timer_get_time() / 1000;
            HandleMoveWakePressDown(current_time, last_trigger_time, gesture_state);
        });

        move_wake_button_.OnPressUp([this]() {
            int64_t current_time = esp_timer_get_time() / 1000;
            HandleMoveWakePressUp(current_time, last_trigger_time, gesture_state);
        });
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

#ifdef SH1106
    void ApplySh1106PanelConfig() {
        // The managed SH1106 driver maps mirror_x to display reverse, so apply
        // the VQ2 panel orientation and color mode explicitly after LVGL setup.
        constexpr uint8_t kSetDisplayNormal = 0xA6;
        constexpr uint8_t kSetDisplayReverse = 0xA7;
        constexpr uint8_t kSetComScanNormal = 0xC0;
        constexpr uint8_t kSetComScanReverse = 0xC8;
        constexpr uint8_t kSetSegmentRemapNormal = 0xA0;
        constexpr uint8_t kSetSegmentRemapInverse = 0xA1;

        ESP_LOGI(TAG, "Apply VQ2 SH1106 config: segment_inverse=%d com_reverse=%d reverse_color=%d",
            DISPLAY_SEGMENT_REMAP_INVERSE, DISPLAY_COM_SCAN_REVERSE, DISPLAY_REVERSE_COLOR);
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_,
            DISPLAY_SEGMENT_REMAP_INVERSE ? kSetSegmentRemapInverse : kSetSegmentRemapNormal, nullptr, 0));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_,
            DISPLAY_COM_SCAN_REVERSE ? kSetComScanReverse : kSetComScanNormal, nullptr, 0));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_,
            DISPLAY_REVERSE_COLOR ? kSetDisplayReverse : kSetDisplayNormal, nullptr, 0));
    }
#endif

    void InitializeOledDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_LOGI(TAG, "Install SH1106 OLED driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_LOGI(TAG, "Install SSD1306 OLED driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "OLED init failed; using NoDisplay fallback");
            display_ = new NoDisplay();
            return;
        }

        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        auto display = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display->SetClockMicroAnimationEnabled(true);
        display_ = display;
#ifdef SH1106
        ApplySh1106PanelConfig();
#endif
    }

    void InitializeDogMotion() {
#if XIAO_XING_VQ2_ENABLE_DOG_MOTION
        if (!IsValidGpio(FL_GPIO_NUM) || !IsValidGpio(FR_GPIO_NUM) ||
                !IsValidGpio(BL_GPIO_NUM) || !IsValidGpio(BR_GPIO_NUM)) {
            ESP_LOGW(TAG, "Dog motion is enabled but one or more servo GPIOs are invalid");
            return;
        }

        servo_dog_ctrl_config_t config = {
            .fl_gpio_num = FL_GPIO_NUM,
            .fr_gpio_num = FR_GPIO_NUM,
            .bl_gpio_num = BL_GPIO_NUM,
            .br_gpio_num = BR_GPIO_NUM,
        };
        servo_dog_ctrl_init(&config);
        dog_motion_enabled_ = true;
#else
        ESP_LOGI(TAG, "Dog motion disabled until VQ2 servo GPIOs are confirmed");
#endif
    }

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
    esp_err_t ConfigureLedStrip(gpio_num_t gpio, bool clear_previous) {
        if (!IsValidGpio(gpio)) {
            ESP_LOGW(TAG, "RGB LED strip disabled because GPIO is not configured");
            return ESP_ERR_INVALID_ARG;
        }

        if (clear_previous && led_strip_ != nullptr) {
            led_strip_clear(led_strip_);
            led_strip_del(led_strip_);
            led_strip_ = nullptr;
            active_led_gpio_ = GPIO_NUM_NC;
        }

        led_strip_config_t strip_config = {
            .strip_gpio_num = gpio,
            .max_leds = RGB_LED_COUNT,
            .led_model = LED_MODEL_WS2812,
            .flags = {
                .invert_out = false,
            },
        };
        led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .mem_block_symbols = 0,
            .flags = {
                .with_dma = false,
            },
        };

        esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize RGB LED strip on GPIO%d: %s",
                static_cast<int>(gpio), esp_err_to_name(ret));
            return ret;
        }

        active_led_gpio_ = gpio;
        ESP_LOGI(TAG, "RGB LED strip initialized on GPIO%d, count=%d",
            static_cast<int>(active_led_gpio_), RGB_LED_COUNT);
        return led_strip_clear(led_strip_);
    }

    void InitializeLedStrip() {
        ConfigureLedStrip(RGB_LED_GPIO, false);
    }

    static uint8_t ScaleLedChannel(uint8_t value, uint8_t brightness) {
        return static_cast<uint8_t>((static_cast<uint16_t>(value) * brightness) / 100);
    }

    esp_err_t ApplyLedState() {
        if (led_strip_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        if (led_on_ && led_brightness_ > 0) {
            r = ScaleLedChannel(led_red_, led_brightness_);
            g = ScaleLedChannel(led_green_, led_brightness_);
            b = ScaleLedChannel(led_blue_, led_brightness_);
        }

        esp_err_t ret = ESP_OK;
        for (int i = 0; i < RGB_LED_COUNT; ++i) {
            ret |= led_strip_set_pixel(led_strip_, i, r, g, b);
        }
        ret |= led_strip_refresh(led_strip_);
        return ret;
    }

    esp_err_t SetLedColor(uint8_t r, uint8_t g, uint8_t b) {
        led_red_ = r;
        led_green_ = g;
        led_blue_ = b;
        led_on_ = (r != 0 || g != 0 || b != 0);
        if (led_on_ && led_brightness_ == 0) {
            led_brightness_ = 100;
        }
        return ApplyLedState();
    }

    esp_err_t TurnLedOn() {
        if (led_brightness_ == 0) {
            led_brightness_ = 100;
        }
        if (led_red_ == 0 && led_green_ == 0 && led_blue_ == 0) {
            led_red_ = 0xFF;
            led_green_ = 0xFF;
            led_blue_ = 0xFF;
        }
        led_on_ = true;
        return ApplyLedState();
    }

    esp_err_t TurnLedOff() {
        led_on_ = false;
        return ApplyLedState();
    }

    esp_err_t SetLedBrightness(int brightness) {
        if (brightness < 0) {
            brightness = 0;
        } else if (brightness > 100) {
            brightness = 100;
        }
        led_brightness_ = static_cast<uint8_t>(brightness);
        if (led_brightness_ == 0) {
            led_on_ = false;
        }
        return ApplyLedState();
    }
#endif

    bool SendDogAction(servo_dog_state_t state, int repeat_count, int speed) {
        if (!dog_motion_enabled_) {
            return false;
        }

        dog_action_args_t args = {
            .repeat_count = NOT_USE,
            .speed = NOT_USE,
            .hold_time_ms = NOT_USE,
            .angle_offset = NOT_USE,
        };
        dog_action_args_t* args_ptr = &args;
        int resolved_repeat_count = repeat_count;
        int resolved_speed = speed > 0 ? speed : kBasicMotionSpeed;

        switch (state) {
        case DOG_STATE_FORWARD:
        case DOG_STATE_BACKWARD:
        case DOG_STATE_TURN_LEFT:
        case DOG_STATE_TURN_RIGHT: {
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kBasicMotionRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.speed = static_cast<int16_t>(resolved_speed);
            break;
        }
        case DOG_STATE_BOW:
        case DOG_STATE_LEAN_BACK:
            args.speed = static_cast<int16_t>(resolved_speed);
            args.hold_time_ms = kGestureHoldTimeMs;
            break;
        case DOG_STATE_BOW_LEAN:
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kBowLeanRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.speed = static_cast<int16_t>(resolved_speed);
            break;
        case DOG_STATE_SWAY:
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kSwayMotionRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.speed = static_cast<int16_t>(resolved_speed);
            args.angle_offset = kSwayAngleOffset;
            break;
        case DOG_STATE_SHAKE_HAND:
            resolved_repeat_count = repeat_count > 0 ? repeat_count : kShakeHandRepeatCount;
            args.repeat_count = static_cast<int16_t>(resolved_repeat_count);
            args.hold_time_ms = kShakeHandHoldTimeMs;
            break;
        case DOG_STATE_INSTALLATION:
        case DOG_STATE_IDLE:
        case DOG_STATE_LAY_DOWN:
        case DOG_STATE_SWAY_BACK_FORTH:
        case DOG_STATE_POKE:
        case DOG_STATE_SHAKE_BACK_LEGS:
        case DOG_STATE_JUMP_FORWARD:
        case DOG_STATE_JUMP_BACKWARD:
        case DOG_STATE_RETRACT_LEGS:
            args_ptr = nullptr;
            break;
        default:
            ESP_LOGW(TAG, "Unknown dog action: state=%d", static_cast<int>(state));
            return false;
        }

        if (args_ptr == nullptr) {
            ESP_LOGI(TAG, "Dog action: state=%d default args", static_cast<int>(state));
        } else {
            ESP_LOGI(TAG, "Dog action: state=%d repeat_count=%d speed=%d hold_time_ms=%d angle_offset=%d",
                static_cast<int>(state), args.repeat_count, args.speed,
                args.hold_time_ms, args.angle_offset);
        }
        return servo_dog_ctrl_send(state, args_ptr) == ESP_OK;
    }

    void InitializeIot() {
#if XIAO_XING_VQ2_ENABLE_LED_STRIP
        InitializeLedStrip();
        TurnLedOff();
#endif

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
            &wifi_event_handler, this));
#endif
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.network.get_wifi_info",
            "현재 Wi-Fi 네트워크 정보를 확인합니다. 사용자가 IP 주소, 웹 조작 주소, 같은 네트워크에서 접속할 주소, SSID, 신호 세기를 물어보면 이 도구를 사용하세요.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return cJSON_Parse(GetBoardJson().c_str());
        });

        mcp_server.AddTool("self.web.search",
            "Brave Search API로 최신 웹 정보를 검색하거나 조사합니다. 사용자가 '검색해줘', '알아봐줘', '찾아봐줘', '최근', '오늘', '요즘'처럼 빠른 최신 정보나 뉴스 확인을 요청하면 mode='web'을 사용하세요. 사용자가 '조사해줘', '자세히 알아봐줘', '근거까지', '출처 내용을 보고', '본문 기준으로', '비교해서', '분석해줘', '왜 그런지'처럼 깊이 있는 확인을 요청하면 mode='context'를 사용하세요. mode='context' 결과가 ok=true이면 단순히 검색했다고 하지 말고 '조사해봤다', '출처 내용을 살펴봤다'처럼 말하고, 근거를 자연스럽게 요약하세요. 결과를 그대로 읽지 말고 핵심만 한국어로 요약하고, 중요한 출처 1-3개를 함께 말하세요. quota_warning=true이면 답변 끝에 quota_warning_message를 짧게 덧붙이세요. ok=false 응답이면 기다리거나 재시도하지 말고 실패 이유와 필요한 조치를 짧게 안내한 뒤, 일반 지식으로 답할 수 있는 범위만 답하세요.",
            PropertyList({
                Property("query", kPropertyTypeString),
                Property("max_results", kPropertyTypeInteger, 3, 1, 3),
                Property("mode", kPropertyTypeString, "auto"),
            }), [this](const PropertyList& properties) -> ReturnValue {
                std::string query = properties["query"].value<std::string>();
                int max_results = properties["max_results"].value<int>();
                std::string mode = properties["mode"].value<std::string>();
                return SearchBrave(query, max_results, mode);
            });

        mcp_server.AddTool("self.web.search.get_config_status",
            "Brave Search 설정 상태를 확인합니다. 실제 API 키 문자열은 이 도구의 반환값에 존재하지 않고 configured 여부만 반환됩니다.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return GetWebSearchConfigStatus();
            });

        mcp_server.AddTool("self.dog.basic_control", "로봇 강아지의 기본 동작 제어. 사용할 수 있는 동작:\n"
            "forward: 앞으로 이동\nbackward: 뒤로 이동\nturn_left: 왼쪽으로 회전\n"
            "turn_right: 오른쪽으로 회전\nstop: 현재 동작 즉시 정지\n"
            "speed는 동작 속도이며 20=느리게, 30=보통, 40=빠르게입니다. 기본값은 30입니다.\n"
            "repeat_count는 이동/회전 반복 횟수이며 1이 기본값입니다.",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("speed", kPropertyTypeInteger, kBasicMotionSpeed, 20, 40),
                Property("repeat_count", kPropertyTypeInteger, kBasicMotionRepeatCount, 1, 4),
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (!dog_motion_enabled_) {
                    return false;
                }
                const std::string& action = properties["action"].value<std::string>();
                int speed = properties["speed"].value<int>();
                int repeat_count = properties["repeat_count"].value<int>();
                if (action == "forward") {
                    return SendDogAction(DOG_STATE_FORWARD, repeat_count, speed);
                } else if (action == "backward") {
                    return SendDogAction(DOG_STATE_BACKWARD, repeat_count, speed);
                } else if (action == "turn_left") {
                    return SendDogAction(DOG_STATE_TURN_LEFT, repeat_count, speed);
                } else if (action == "turn_right") {
                    return SendDogAction(DOG_STATE_TURN_RIGHT, repeat_count, speed);
                } else if (action == "stop") {
                    return SendDogAction(DOG_STATE_IDLE, 0, 0);
                } else {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.dog.advanced_control", "로봇 강아지의 확장 동작 제어. 사용할 수 있는 동작:\n"
            "sway_back_forth: 앞뒤로 흔들기\nlay_down: 엎드리기\nsway: 좌우로 흔들기\n"
            "bow: 인사하기\nlean_back: 뒤로 젖히기\nbow_lean: 앞뒤로 인사하기\n"
            "retract_legs: 다리 접기\nshake_hand: 손 흔들기\npoke: 찌르기\n"
            "shake_back_legs: 뒷다리 흔들기\njump_forward: 앞으로 점프\n"
            "jump_backward: 뒤로 점프\n"
            "speed는 지원되는 동작의 속도이며 20=느리게, 30=보통, 40=빠르게입니다. 기본값은 30입니다.\n"
            "repeat_count는 지원되는 동작의 반복 횟수이며 0이면 동작별 기본값을 사용합니다.",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("speed", kPropertyTypeInteger, kBasicMotionSpeed, 20, 40),
                Property("repeat_count", kPropertyTypeInteger, 0, 0, 10),
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (!dog_motion_enabled_) {
                    return false;
                }
                const std::string& action = properties["action"].value<std::string>();
                int speed = properties["speed"].value<int>();
                int repeat_count = properties["repeat_count"].value<int>();
                if (action == "sway_back_forth") {
                    return SendDogAction(DOG_STATE_SWAY_BACK_FORTH, repeat_count, speed);
                } else if (action == "lay_down") {
                    return SendDogAction(DOG_STATE_LAY_DOWN, repeat_count, speed);
                } else if (action == "sway") {
                    return SendDogAction(DOG_STATE_SWAY, repeat_count, speed);
                } else if (action == "bow") {
                    return SendDogAction(DOG_STATE_BOW, repeat_count, speed);
                } else if (action == "lean_back") {
                    return SendDogAction(DOG_STATE_LEAN_BACK, repeat_count, speed);
                } else if (action == "bow_lean") {
                    return SendDogAction(DOG_STATE_BOW_LEAN, repeat_count, speed);
                } else if (action == "retract_legs") {
                    return SendDogAction(DOG_STATE_RETRACT_LEGS, repeat_count, speed);
                } else if (action == "shake_hand") {
                    return SendDogAction(DOG_STATE_SHAKE_HAND, repeat_count, speed);
                } else if (action == "poke") {
                    return SendDogAction(DOG_STATE_POKE, repeat_count, speed);
                } else if (action == "shake_back_legs") {
                    return SendDogAction(DOG_STATE_SHAKE_BACK_LEGS, repeat_count, speed);
                } else if (action == "jump_forward") {
                    return SendDogAction(DOG_STATE_JUMP_FORWARD, repeat_count, speed);
                } else if (action == "jump_backward") {
                    return SendDogAction(DOG_STATE_JUMP_BACKWARD, repeat_count, speed);
                } else {
                    return false;
                }
            });

#if XIAO_XING_VQ2_ENABLE_LED_STRIP
        mcp_server.AddTool("self.light.get_power", "조명이 켜져 있는지 확인합니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return led_on_;
            });

        mcp_server.AddTool("self.light.turn_on", "조명을 켭니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (TurnLedOn() != ESP_OK) {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.light.turn_off", "조명을 끕니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (TurnLedOff() != ESP_OK) {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.light.get_brightness", "조명 밝기를 0-100 값으로 확인합니다", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                return static_cast<int>(led_brightness_);
            });

        mcp_server.AddTool("self.light.set_brightness", "조명 밝기를 0-100 값으로 설정합니다",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100),
            }), [this](const PropertyList& properties) -> ReturnValue {
                int brightness = properties["brightness"].value<int>();
                if (SetLedBrightness(brightness) != ESP_OK) {
                    return false;
                }
                return true;
            });

        mcp_server.AddTool("self.light.set_rgb", "RGB 조명 색상을 설정합니다",
            PropertyList({
                Property("r", kPropertyTypeInteger, 0, 255),
                Property("g", kPropertyTypeInteger, 0, 255),
                Property("b", kPropertyTypeInteger, 0, 255),
            }), [this](const PropertyList& properties) -> ReturnValue {
                int r = properties["r"].value<int>();
                int g = properties["g"].value<int>();
                int b = properties["b"].value<int>();
                if (SetLedColor(r, g, b) != ESP_OK) {
                    return false;
                }
                return true;
            });
#endif
    }

public:
    XiaoXingVq2() :
        boot_button_(BOOT_BUTTON_GPIO),
        audio_wake_button_(AUDIO_WAKE_BUTTON_GPIO),
        move_wake_button_(MOVE_WAKE_BUTTON_GPIO) {
        InitializeButtons();
        InitializeDisplayI2c();
        InitializeOledDisplay();
        InitializeDogMotion();
        InitializeIot();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
#if XIAO_XING_VQ2_ENABLE_AUDIO
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
#else
        static DummyAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &audio_codec;
#endif
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(XiaoXingVq2);
