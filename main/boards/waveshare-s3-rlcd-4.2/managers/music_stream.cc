#include "music_stream.h"

#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "settings.h"

MusicStream& MusicStream::GetInstance() {
    static MusicStream instance;
    return instance;
}

// ===== 小工具 =====

// UTF-8 安全的百分号编码（中文歌名必须走这一步，否则查询参数会被服务端截断）
static std::string UrlEncode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// 一次 GET，把响应体读进 std::string。
// 用 open/fetch_headers/read 这套而不是事件回调：中转服务的响应只有几 KB，
// 直接同步读最简单，也不会碰上回调里不能分配内存的限制。
static bool HttpGet(const std::string& url, std::string* body, int timeout_ms, std::string* err) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = timeout_ms;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // 允许直接连 https 的中转（自己套 nginx 时用得上）
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 3;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        if (err) *err = "HTTP 客户端初始化失败";
        return false;
    }

    bool ok = false;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            if (err) *err = "连不上中转服务（地址或网络不通）";
            break;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            char buf[80];
            snprintf(buf, sizeof(buf), "中转服务返回 HTTP %d", status);
            if (err) *err = buf;
            break;
        }

        body->clear();
        char chunk[1024];
        while (true) {
            const int got = esp_http_client_read(client, chunk, sizeof(chunk));
            if (got <= 0) {
                break;
            }
            body->append(chunk, static_cast<size_t>(got));
            if (body->size() > 96 * 1024) {
                // 歌词 + 搜索结果正常只有几 KB，超过 96KB 一定是配置错了
                ESP_LOGW("MusicStream", "响应异常大（>96KB），截断");
                break;
            }
        }
        ok = !body->empty();
        if (!ok && err) *err = "中转服务返回空响应";
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static std::string JsonStr(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return "";
}

// ===== 配置 =====

void MusicStream::SetBase(const std::string& base) {
    std::string v = base;
    // 去掉结尾的 '/'，拼路径时统一加
    while (!v.empty() && (v.back() == '/' || v.back() == ' ')) {
        v.pop_back();
    }
    // 没写协议就默认 http（局域网中转绝大多数是裸 http）
    if (!v.empty() && v.find("://") == std::string::npos) {
        v = "http://" + v;
    }
    base_ = v;
    base_loaded_ = true;

    Settings settings(kNvsNs, true);
    settings.SetString("base", base_);
    ESP_LOGI(TAG, "音乐中转地址已保存: %s", base_.c_str());
}

std::string MusicStream::Base() {
    if (!base_loaded_) {
        Settings settings(kNvsNs);
        base_ = settings.GetString("base", "");
        base_loaded_ = true;
    }
    return base_;
}

bool MusicStream::Configured() {
    return !Base().empty();
}

// ===== 接口 =====

bool MusicStream::Health(bool* logged_in, std::string* err) {
    if (!Configured()) {
        if (err) *err = "还没设置中转服务地址";
        return false;
    }
    std::string body;
    if (!HttpGet(Base() + "/health", &body, 5000, err)) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        if (err) *err = "中转服务返回的不是 JSON（地址指向了错误的服务？）";
        return false;
    }
    const bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
    if (logged_in != nullptr) {
        const cJSON* li = cJSON_GetObjectItemCaseSensitive(root, "logged_in");
        *logged_in = cJSON_IsTrue(li);
    }
    if (!ok && err) *err = "中转服务自检未通过";
    cJSON_Delete(root);
    return ok;
}

bool MusicStream::Search(const std::string& keyword, int limit,
                         std::vector<Song>* out, std::string* err) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    if (!Configured()) {
        if (err) *err = "还没设置中转服务地址";
        return false;
    }
    if (limit <= 0) limit = 8;

    char url[512];
    snprintf(url, sizeof(url), "%s/search?kw=%s&limit=%d",
             Base().c_str(), UrlEncode(keyword).c_str(), limit);

    std::string body;
    if (!HttpGet(url, &body, 8000, err)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        if (err) *err = "搜索响应解析失败";
        return false;
    }

    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
    if (!ok) {
        const std::string reason = JsonStr(root, "reason");
        if (err) *err = reason.empty() ? "中转服务搜索失败" : reason;
        cJSON_Delete(root);
        return false;
    }

    const cJSON* songs = cJSON_GetObjectItemCaseSensitive(root, "songs");
    if (cJSON_IsArray(songs)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, songs) {
            Song s;
            const cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (cJSON_IsNumber(id)) {
                s.id = std::to_string(static_cast<long long>(id->valuedouble));
            } else if (cJSON_IsString(id)) {
                s.id = id->valuestring;
            }
            s.title = JsonStr(it, "title");
            s.artist = JsonStr(it, "artist");
            const cJSON* dur = cJSON_GetObjectItemCaseSensitive(it, "duration_ms");
            if (cJSON_IsNumber(dur)) {
                s.duration_ms = static_cast<int>(dur->valuedouble);
            }
            if (!s.title.empty()) {
                out->push_back(s);
            }
        }
    }
    cJSON_Delete(root);
    return !out->empty();
}

MusicStream::Resolved MusicStream::Play(const std::string& keyword) {
    Resolved r;
    if (!Configured()) {
        r.reason = "还没设置音乐中转服务地址（要在一台常开的电脑上跑 tools/netease-relay/relay.py）";
        return r;
    }
    if (keyword.empty()) {
        r.reason = "没说要听什么歌";
        return r;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/play?kw=%s", Base().c_str(), UrlEncode(keyword).c_str());

    std::string body;
    std::string err;
    if (!HttpGet(url, &body, 15000, &err)) {
        r.reason = err.empty() ? "请求中转服务失败" : err;
        return r;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        r.reason = "中转服务返回的数据无法解析";
        return r;
    }

    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
        const std::string reason = JsonStr(root, "reason");
        r.reason = reason.empty() ? "这首歌拿不到播放地址（可能需要会员、或没有版权）" : reason;
        cJSON_Delete(root);
        return r;
    }

    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id)) {
        r.song.id = std::to_string(static_cast<long long>(id->valuedouble));
    } else if (cJSON_IsString(id)) {
        r.song.id = id->valuestring;
    }
    r.song.title = JsonStr(root, "title");
    r.song.artist = JsonStr(root, "artist");
    const cJSON* dur = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    if (cJSON_IsNumber(dur)) {
        r.song.duration_ms = static_cast<int>(dur->valuedouble);
    }
    r.url = JsonStr(root, "url");
    r.lyric = JsonStr(root, "lyric");

    cJSON_Delete(root);

    if (r.url.empty()) {
        r.reason = "中转服务没返回播放地址";
        return r;
    }
    if (r.song.title.empty()) {
        r.song.title = keyword;
    }
    r.ok = true;
    return r;
}
