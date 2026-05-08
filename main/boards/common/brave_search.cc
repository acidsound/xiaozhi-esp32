#include "brave_search.h"

#include "board.h"
#include "settings.h"
#include "sdkconfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <nvs_flash.h>

#if CONFIG_IDF_TARGET_ESP32C3
#include <esp_crt_bundle.h>
#include <esp_tls.h>
#endif

#define TAG "BraveSearch"

namespace {

constexpr int kWebSearchTimeoutMs = 6000;
#if CONFIG_IDF_TARGET_ESP32C3
constexpr size_t kWebSearchMaxBodyBytes = 8 * 1024;
constexpr int kWebSearchMaxResults = 1;
constexpr size_t kCompactTitleMaxBytes = 120;
constexpr size_t kCompactUrlMaxBytes = 180;
constexpr size_t kCompactDescriptionMaxBytes = 240;
constexpr const char* kBraveSearchHost = "api.search.brave.com";
constexpr int kBraveSearchPort = 443;
constexpr size_t kHttpHeaderMaxBytes = 3072;
constexpr size_t kChunkLineMaxBytes = 32;
#else
constexpr size_t kWebSearchMaxBodyBytes = 64 * 1024;
constexpr int kWebSearchMaxResults = 3;
#endif
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

#if CONFIG_IDF_TARGET_ESP32C3

void AppendUtf8Codepoint(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool ParseHex4(const std::string& json, size_t pos, uint32_t& value) {
    if (pos + 4 > json.size()) {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        int digit = HexValue(json[pos + i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return true;
}

void AppendJsonString(std::string& out, const std::string& value) {
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    out += escaped;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

bool ParseJsonStringAt(const std::string& json, size_t quote_pos, std::string& out) {
    if (quote_pos >= json.size() || json[quote_pos] != '"') {
        return false;
    }

    out.clear();
    for (size_t i = quote_pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= json.size()) {
            return false;
        }

        switch (json[i]) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!ParseHex4(json, i + 1, codepoint)) {
                    return false;
                }
                i += 4;
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF
                    && i + 6 < json.size()
                    && json[i + 1] == '\\'
                    && json[i + 2] == 'u') {
                    uint32_t low = 0;
                    if (ParseHex4(json, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF) {
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                AppendUtf8Codepoint(out, codepoint);
                break;
            }
            default:
                out.push_back(json[i]);
                break;
        }
    }
    return false;
}

bool ExtractJsonStringField(const std::string& json, size_t start, const char* key, std::string& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";

    size_t key_pos = json.find(needle, start);
    if (key_pos == std::string::npos) {
        return false;
    }

    size_t colon_pos = json.find(':', key_pos + needle.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    size_t value_pos = colon_pos + 1;
    while (value_pos < json.size() && std::isspace(static_cast<unsigned char>(json[value_pos]))) {
        value_pos++;
    }
    return ParseJsonStringAt(json, value_pos, out);
}

bool ExtractFirstBraveWebResult(const std::string& body, std::string& title,
                                std::string& url, std::string& description) {
    size_t web_pos = body.find("\"web\"");
    if (web_pos == std::string::npos) {
        web_pos = 0;
    }
    size_t results_pos = body.find("\"results\"", web_pos);
    if (results_pos == std::string::npos) {
        return false;
    }
    size_t array_pos = body.find('[', results_pos);
    size_t object_pos = body.find('{', array_pos == std::string::npos ? results_pos : array_pos);
    if (object_pos == std::string::npos) {
        return false;
    }

    if (!ExtractJsonStringField(body, object_pos, "title", title)
        || !ExtractJsonStringField(body, object_pos, "url", url)) {
        return false;
    }
    ExtractJsonStringField(body, object_pos, "description", description);
    TrimUtf8ToBytes(title, kCompactTitleMaxBytes);
    TrimUtf8ToBytes(url, kCompactUrlMaxBytes);
    TrimUtf8ToBytes(description, kCompactDescriptionMaxBytes);
    return !title.empty() && !url.empty();
}

std::string MakeCompactWebSearchError(const std::string& code, const std::string& message,
                                      int status_code = -1) {
    std::string out;
    out.reserve(code.size() + message.size() + 96);
    out += "{\"ok\":false,\"code\":";
    AppendJsonString(out, code);
    out += ",\"error\":";
    AppendJsonString(out, message);
    if (status_code >= 0) {
        out += ",\"status_code\":";
        out += std::to_string(status_code);
    }
    out += ",\"fallback_instruction\":\"사용자에게 실패 이유를 짧게 설명하고 API 키/네트워크 확인 후 다시 요청해 달라고 안내하세요.\"}";
    return out;
}

bool AppendBodyLimited(std::string& body, const char* data, size_t len,
                       int status_code, std::string& error_text) {
    if (len == 0) {
        return true;
    }
    if (body.size() + len > kWebSearchMaxBodyBytes) {
        error_text = MakeCompactWebSearchError("response_too_large",
            "Brave Search API 응답이 ESP32-C3에서 처리하기에는 너무 큽니다.", status_code);
        return false;
    }
    body.append(data, len);
    return true;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

int ParseHttpStatusCode(const std::string& headers) {
    size_t first_space = headers.find(' ');
    if (first_space == std::string::npos || first_space + 4 > headers.size()) {
        return -1;
    }
    return std::atoi(headers.c_str() + first_space + 1);
}

std::string GetHeaderValueLower(const std::string& headers, const char* key) {
    std::string lower = ToLowerAscii(headers);
    std::string needle = "\n";
    needle += key;
    needle += ":";
    size_t pos = lower.find(needle);
    if (pos == std::string::npos) {
        std::string start_needle = std::string(key) + ":";
        if (lower.rfind(start_needle, 0) != 0) {
            return "";
        }
        pos = 0;
    } else {
        pos += 1;
    }

    size_t value_start = lower.find(':', pos);
    if (value_start == std::string::npos) {
        return "";
    }
    value_start++;
    while (value_start < lower.size() && std::isspace(static_cast<unsigned char>(lower[value_start]))) {
        value_start++;
    }

    size_t value_end = lower.find('\n', value_start);
    if (value_end == std::string::npos) {
        value_end = lower.size();
    }
    while (value_end > value_start && std::isspace(static_cast<unsigned char>(lower[value_end - 1]))) {
        value_end--;
    }
    return lower.substr(value_start, value_end - value_start);
}

size_t ParseContentLength(const std::string& headers) {
    std::string value = GetHeaderValueLower(headers, "content-length");
    if (value.empty()) {
        return static_cast<size_t>(-1);
    }
    char* end = nullptr;
    unsigned long length = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return static_cast<size_t>(-1);
    }
    return static_cast<size_t>(length);
}

bool IsChunkedResponse(const std::string& headers) {
    return GetHeaderValueLower(headers, "transfer-encoding").find("chunked") != std::string::npos;
}

class ChunkedBodyReader {
public:
    bool Feed(const char* data, size_t len, int status_code, std::string& body,
              std::string& error_text, bool& complete) {
        size_t pos = 0;
        while (pos < len && !complete) {
            if (state_ == State::kSize) {
                char c = data[pos++];
                if (c == '\n') {
                    if (!ParseSizeLine(error_text, status_code)) {
                        return false;
                    }
                    line_.clear();
                    if (remaining_ == 0) {
                        complete = true;
                        return true;
                    }
                    state_ = State::kData;
                } else if (line_.size() < kChunkLineMaxBytes) {
                    line_.push_back(c);
                } else {
                    error_text = MakeCompactWebSearchError("invalid_chunk",
                        "Brave Search API chunk 크기를 해석할 수 없습니다.", status_code);
                    return false;
                }
            } else if (state_ == State::kData) {
                size_t take = std::min(remaining_, len - pos);
                if (!AppendBodyLimited(body, data + pos, take, status_code, error_text)) {
                    return false;
                }
                pos += take;
                remaining_ -= take;
                if (remaining_ == 0) {
                    state_ = State::kDataCrLf;
                    crlf_left_ = 2;
                }
            } else {
                size_t take = std::min(crlf_left_, len - pos);
                pos += take;
                crlf_left_ -= take;
                if (crlf_left_ == 0) {
                    state_ = State::kSize;
                }
            }
        }
        return true;
    }

private:
    enum class State {
        kSize,
        kData,
        kDataCrLf,
    };

    bool ParseSizeLine(std::string& error_text, int status_code) {
        while (!line_.empty() && (line_.back() == '\r' || std::isspace(static_cast<unsigned char>(line_.back())))) {
            line_.pop_back();
        }
        size_t semi = line_.find(';');
        std::string size_text = semi == std::string::npos ? line_ : line_.substr(0, semi);
        size_t start = 0;
        while (start < size_text.size() && std::isspace(static_cast<unsigned char>(size_text[start]))) {
            start++;
        }
        char* end = nullptr;
        unsigned long value = std::strtoul(size_text.c_str() + start, &end, 16);
        if (end == size_text.c_str() + start) {
            error_text = MakeCompactWebSearchError("invalid_chunk",
                "Brave Search API chunk 크기를 해석할 수 없습니다.", status_code);
            return false;
        }
        remaining_ = static_cast<size_t>(value);
        return true;
    }

    State state_ = State::kSize;
    std::string line_;
    size_t remaining_ = 0;
    size_t crlf_left_ = 0;
};

int GetTlsLastError(esp_tls_t* tls) {
    esp_tls_error_handle_t last_error = nullptr;
    if (tls != nullptr && esp_tls_get_error_handle(tls, &last_error) == ESP_OK && last_error != nullptr) {
        int error_code = 0;
        int error_flags = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
        return static_cast<int>(err);
    }
    return -1;
}

bool TlsWriteAll(esp_tls_t* tls, const std::string& data, std::string& error_text) {
    size_t sent = 0;
    while (sent < data.size()) {
        int ret = esp_tls_conn_write(tls, data.data() + sent, data.size() - sent);
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret <= 0) {
            error_text = MakeCompactWebSearchError("write_failed",
                "Brave Search API 요청 전송에 실패했습니다: " + std::to_string(ret));
            return false;
        }
        sent += static_cast<size_t>(ret);
    }
    return true;
}

bool FeedPlainBody(const char* data, size_t len, size_t content_length,
                   size_t& body_received, int status_code, std::string& body,
                   std::string& error_text, bool& complete) {
    size_t take = len;
    if (content_length != static_cast<size_t>(-1)) {
        if (body_received >= content_length) {
            complete = true;
            return true;
        }
        take = std::min(take, content_length - body_received);
    }

    if (!AppendBodyLimited(body, data, take, status_code, error_text)) {
        return false;
    }
    body_received += take;
    if (content_length != static_cast<size_t>(-1) && body_received >= content_length) {
        complete = true;
    }
    return true;
}

bool ReadBraveWebDirect(const std::string& path, const std::string& api_key,
                        int& status_code, std::string& body, std::string& error_text) {
    esp_tls_t* tls = esp_tls_init();
    if (tls == nullptr) {
        error_text = MakeCompactWebSearchError("out_of_memory", "TLS 클라이언트를 초기화할 메모리가 부족합니다.");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kWebSearchTimeoutMs;

    int ret = esp_tls_conn_new_sync(kBraveSearchHost, std::strlen(kBraveSearchHost),
                                    kBraveSearchPort, &cfg, tls);
    if (ret != 1) {
        int last_error = GetTlsLastError(tls);
        esp_tls_conn_destroy(tls);
        error_text = MakeCompactWebSearchError("connection_failed",
            "Brave Search API 연결에 실패했습니다: " + std::to_string(last_error));
        return false;
    }

    std::string request;
    request.reserve(path.size() + api_key.size() + 192);
    request += "GET ";
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += kBraveSearchHost;
    request += "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\nX-Subscription-Token: ";
    request += api_key;
    request += "\r\n\r\n";

    if (!TlsWriteAll(tls, request, error_text)) {
        esp_tls_conn_destroy(tls);
        return false;
    }
    request.clear();
    request.shrink_to_fit();

    std::string headers;
    headers.reserve(1024);
    ChunkedBodyReader chunked_reader;
    bool headers_complete = false;
    bool chunked = false;
    bool complete = false;
    size_t content_length = static_cast<size_t>(-1);
    size_t body_received = 0;
    char buffer[512];

    while (!complete) {
        ret = esp_tls_conn_read(tls, buffer, sizeof(buffer));
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret < 0) {
            esp_tls_conn_destroy(tls);
            error_text = MakeCompactWebSearchError("read_timeout",
                "Brave Search API 응답을 읽는 중 오류가 발생했습니다: " + std::to_string(ret), status_code);
            return false;
        }
        if (ret == 0) {
            break;
        }

        const char* data = buffer;
        size_t len = static_cast<size_t>(ret);
        if (!headers_complete) {
            if (headers.size() + len > kHttpHeaderMaxBytes + kWebSearchMaxBodyBytes) {
                esp_tls_conn_destroy(tls);
                error_text = MakeCompactWebSearchError("header_too_large",
                    "Brave Search API 응답 헤더가 너무 큽니다.");
                return false;
            }
            headers.append(data, len);
            size_t header_end = headers.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (headers.size() > kHttpHeaderMaxBytes) {
                    esp_tls_conn_destroy(tls);
                    error_text = MakeCompactWebSearchError("header_too_large",
                        "Brave Search API 응답 헤더가 너무 큽니다.");
                    return false;
                }
                continue;
            }

            size_t body_start = header_end + 4;
            status_code = ParseHttpStatusCode(headers);
            if (status_code < 100) {
                esp_tls_conn_destroy(tls);
                error_text = MakeCompactWebSearchError("invalid_response",
                    "Brave Search API 응답 상태줄을 해석할 수 없습니다.");
                return false;
            }
            chunked = IsChunkedResponse(headers);
            content_length = ParseContentLength(headers);
            headers_complete = true;

            const char* leftover_data = headers.data() + body_start;
            size_t leftover_len = headers.size() - body_start;
            bool ok = true;
            if (leftover_len > 0) {
                ok = chunked
                    ? chunked_reader.Feed(leftover_data, leftover_len, status_code, body, error_text, complete)
                    : FeedPlainBody(leftover_data, leftover_len, content_length, body_received, status_code, body, error_text, complete);
            }
            headers.clear();
            headers.shrink_to_fit();
            if (!ok) {
                esp_tls_conn_destroy(tls);
                return false;
            }
            continue;
        }

        bool ok = chunked
            ? chunked_reader.Feed(data, len, status_code, body, error_text, complete)
            : FeedPlainBody(data, len, content_length, body_received, status_code, body, error_text, complete);
        if (!ok) {
            esp_tls_conn_destroy(tls);
            return false;
        }
    }

    esp_tls_conn_destroy(tls);
    if (!headers_complete) {
        error_text = MakeCompactWebSearchError("header_timeout",
            "Brave Search API 응답 헤더를 받지 못했습니다.");
        return false;
    }
    return true;
}
#endif

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

    max_results = std::clamp(max_results, 1, kWebSearchMaxResults);
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

#if CONFIG_IDF_TARGET_ESP32C3
std::string SearchBraveWebText(const std::string& query, int max_results, const std::string& api_key) {
    if (query.empty()) {
        return MakeCompactWebSearchError("empty_query", "검색어가 비어 있습니다.");
    }

    if (api_key.empty()) {
        return MakeCompactWebSearchError("missing_api_key",
            "Brave Web Search API 키가 설정되어 있지 않습니다. 웹 설정 화면의 Settings > Web Search에서 API 키를 저장하세요.");
    }

    max_results = std::clamp(max_results, 1, kWebSearchMaxResults);
    std::string path = "/res/v1/web/search?q=" + UrlEncode(query)
        + "&count=" + std::to_string(max_results)
        + "&country=kr&search_lang=ko&spellcheck=1&result_filter=web&text_decorations=false&extra_snippets=false";

    ESP_LOGI(TAG, "Brave compact search: query=%s count=%d", query.c_str(), max_results);
    int status_code = -1;
    std::string body;
    std::string error_text;
    if (!ReadBraveWebDirect(path, api_key, status_code, body, error_text)) {
        return error_text;
    }

    ESP_LOGI(TAG, "Brave compact response: status=%d body=%u bytes",
        status_code, static_cast<unsigned>(body.size()));

    if (status_code != 200) {
        std::string code = status_code == 401 || status_code == 403 ? "auth_failed" :
            (status_code == 429 ? "rate_limited" : "http_error");
        return MakeCompactWebSearchError(code,
            "Brave Search API 오류: HTTP " + std::to_string(status_code), status_code);
    }

    std::string title;
    std::string result_url;
    std::string description;
    if (!ExtractFirstBraveWebResult(body, title, result_url, description)) {
        return MakeCompactWebSearchError("no_results", "검색 결과가 없습니다.", status_code);
    }

    std::string summary_hint = title;
    if (!description.empty()) {
        summary_hint += ": ";
        summary_hint += description;
    }

    std::string out;
    out.reserve(query.size() + title.size() + result_url.size() + description.size() + 192);
    out += "{\"ok\":true,\"provider\":\"brave\",\"query\":";
    AppendJsonString(out, query);
    out += ",\"count\":1,\"results\":[{\"title\":";
    AppendJsonString(out, title);
    out += ",\"url\":";
    AppendJsonString(out, result_url);
    if (!description.empty()) {
        out += ",\"description\":";
        AppendJsonString(out, description);
    }
    out += "}],\"summary_hint\":";
    AppendJsonString(out, summary_hint);
    out += ",\"response_hint\":\"핵심만 한국어로 요약하고 출처 제목과 URL 1개를 함께 말하세요.\"}";
    return out;
}
#endif

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

std::string SearchText(const std::string& query, int max_results, const std::string& mode) {
#if CONFIG_IDF_TARGET_ESP32C3
    if (mode != "web" && mode != "quick" && mode != "auto") {
        ESP_LOGW(TAG, "ESP32-C3 uses compact Web Search instead of mode=%s", mode.c_str());
    }
    return SearchBraveWebText(query, max_results, GetBraveApiKey());
#else
    cJSON* result = Search(query, max_results, mode);
    char* json_str = cJSON_PrintUnformatted(result);
    std::string text = json_str == nullptr ? "{\"ok\":false,\"code\":\"out_of_memory\"}" : json_str;
    if (json_str != nullptr) {
        cJSON_free(json_str);
    }
    cJSON_Delete(result);
    return text;
#endif
}

} // namespace brave_search
