#include "brave_search.h"

#include "board.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "BraveSearch"

namespace {

constexpr int kWebSearchTimeoutMs = 6000;
constexpr size_t kWebSearchMaxBodyBytes = 64 * 1024;
constexpr int kLlmContextMaxResults = 2;
constexpr size_t kLlmContextDescriptionMaxBytes = 360;
constexpr size_t kLlmContextSummaryMaxBytes = 900;
constexpr size_t kLlmContextSummaryLineMaxBytes = 280;
constexpr const char* kWebSearchSettingsNamespace = "web_search";
constexpr const char* kBraveApiKeySetting = "brave_api_key";
constexpr const char* kBraveLlmContextApiKeySetting = "llm_ctx_key";
constexpr const char* kBraveUseLlmContextSetting = "use_llm_ctx";

std::string UrlEncode(const std::string& value) {
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

std::string JsonString(const cJSON* item, const char* key) {
    if (item == nullptr) {
        return "";
    }
    auto value = cJSON_GetObjectItem(item, key);
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return "";
    }
    return value->valuestring;
}

size_t Utf8CharLength(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

void TrimUtf8ToBytes(std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return;
    }

    size_t pos = 0;
    while (pos < value.size()) {
        size_t len = Utf8CharLength(static_cast<unsigned char>(value[pos]));
        if (pos + len > max_bytes) {
            break;
        }
        pos += len;
    }
    value.resize(pos);
}

std::string TrimmedUtf8(std::string value, size_t max_bytes) {
    TrimUtf8ToBytes(value, max_bytes);
    return value;
}

cJSON* MakeWebSearchError(const std::string& code, const std::string& message) {
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

bool IsSettingStringConfigured(const char* key) {
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

void AddHeaderIfPresent(cJSON* root, Http* http, const char* json_key, const char* header_key) {
    std::string value = http->GetResponseHeader(header_key);
    if (!value.empty()) {
        cJSON_AddStringToObject(root, json_key, value.c_str());
    }
}

int ParseRateLimitWindowValue(const std::string& value, int index) {
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

void AddRateLimitHeaders(cJSON* root, Http* http) {
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

bool IsSearchOk(cJSON* response) {
    return cJSON_IsTrue(cJSON_GetObjectItem(response, "ok"));
}

bool IsMonthlyQuotaExhausted(cJSON* response) {
    if (JsonString(response, "code") != "rate_limited") {
        return false;
    }

    int monthly_remaining = ParseRateLimitWindowValue(JsonString(response, "rate_limit_remaining"), 1);
    return monthly_remaining == 0;
}

bool ContainsAny(const std::string& text, const char* const* words, size_t word_count) {
    for (size_t i = 0; i < word_count; ++i) {
        if (text.find(words[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ShouldUseLlmContext(const std::string& query, const std::string& mode) {
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

std::string ExtractBraveErrorCode(const std::string& body) {
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
        auto board_info = cJSON_Parse(Board::GetInstance().GetBoardJson().c_str());
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

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
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

    max_results = std::clamp(max_results, 1, kLlmContextMaxResults);
    std::string url = "https://api.search.brave.com/res/v1/llm/context?q=" + UrlEncode(query)
        + "&count=" + std::to_string(max_results)
        + "&maximum_number_of_urls=" + std::to_string(max_results)
        + "&maximum_number_of_tokens=1024&maximum_number_of_snippets=4"
        + "&maximum_number_of_tokens_per_url=512&maximum_number_of_snippets_per_url=2"
        + "&country=kr&search_lang=ko&context_threshold_mode=strict&enable_local=false";

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
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
            if (description.size() >= kLlmContextDescriptionMaxBytes) {
                TrimUtf8ToBytes(description, kLlmContextDescriptionMaxBytes);
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

        if (summary_hint.size() < kLlmContextSummaryMaxBytes) {
            std::string line = title + ": " + TrimmedUtf8(description, kLlmContextSummaryLineMaxBytes) + "\n";
            if (summary_hint.size() + line.size() > kLlmContextSummaryMaxBytes) {
                line = TrimmedUtf8(line, kLlmContextSummaryMaxBytes - summary_hint.size());
            }
            summary_hint += line;
        }
        added++;
    }

    if (added == 0) {
        cJSON_Delete(result_array);
        cJSON_Delete(response);
        return MakeWebSearchError("no_results", "검색 컨텍스트 결과가 없습니다.");
    }

    cJSON_AddNumberToObject(root, "count", added);
    cJSON_AddBoolToObject(root, "truncated", true);
    cJSON_AddStringToObject(root, "summary_hint", summary_hint.c_str());
    cJSON_AddItemToObject(root, "results", result_array);
    cJSON_Delete(response);
    return root;
}

} // namespace

namespace brave_search {

cJSON* GetConfigStatus() {
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

cJSON* Search(const std::string& query, int max_results, const std::string& mode) {
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

} // namespace brave_search
