#include "photo_web_server.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "photo_loader.h"
#include "sdcard_manager.h"
#include "wifi_manager.h"

static const char* TAG = "PhotoWeb";

// 上传页面。刻意做成单文件、无外部依赖：
// 手机浏览器在弱网下也能秒开，不需要任何 CDN 或前端框架。
static const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>小智相册</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:18px;font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;background:#f5f5f7;color:#1d1d1f}
h1{font-size:20px;margin:0 0 4px}
.sub{font-size:13px;color:#86868b;margin-bottom:16px}
.card{background:#fff;border-radius:14px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.06)}
#drop{border:2px dashed #c7c7cc;border-radius:12px;padding:26px 12px;text-align:center;color:#86868b;font-size:14px;cursor:pointer;transition:.2s}
#drop.on{border-color:#0071e3;background:#f0f7ff;color:#0071e3}
#file{display:none}
.bar{height:6px;background:#e5e5ea;border-radius:3px;overflow:hidden;margin-top:12px;display:none}
.bar i{display:block;height:100%;width:0;background:#0071e3;transition:width .15s}
#log{margin-top:10px;font-size:13px;color:#86868b;min-height:18px;word-break:break-all}
ul{list-style:none;margin:0;padding:0}
li{display:flex;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid #f0f0f2;font-size:14px}
li:last-child{border-bottom:none}
li span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;padding-right:10px}
.del{color:#ff3b30;font-size:13px;background:none;border:none;cursor:pointer;flex:none}
.empty{color:#c7c7cc;font-size:13px;text-align:center;padding:12px 0;display:block}
</style></head><body>
<h1>换成我的图片</h1>
<div class="sub">选一张就行，传完立刻显示在屏幕上。不写 SD 卡，设备重启会回到内置默认图。</div>
<div class="card">
  <div id="drop">点击选择图片<br><span style="font-size:12px">也可以把图片拖进来</span></div>
  <input type="file" id="file" accept="image/*">
  <div class="bar" id="bar"><i id="barin"></i></div>
  <div id="log"></div>
</div>
<div class="card">
  <div style="font-size:14px;font-weight:600;margin-bottom:6px">屏幕当前显示</div>
  <div id="cur" style="font-size:13px;color:#86868b">读取中…</div>
</div>
<script>
var drop=document.getElementById('drop'),file=document.getElementById('file'),
    bar=document.getElementById('bar'),barin=document.getElementById('barin'),
    log=document.getElementById('log'),cur=document.getElementById('cur');
drop.onclick=function(){file.click()};
drop.ondragover=function(e){e.preventDefault();drop.classList.add('on')};
drop.ondragleave=function(){drop.classList.remove('on')};
drop.ondrop=function(e){e.preventDefault();drop.classList.remove('on');up(e.dataTransfer.files)};
file.onchange=function(){up(file.files)};
// 屏幕上的照片位只有 210×248，还是黑白点阵。所以上传前先在浏览器里裁好缩好再发：
//   1) 按 210:248 居中裁剪（和固件里的 cover 裁剪同一套规则，网页上看到的就是屏上的构图）
//   2) 缩到 420×496（2 倍），固件那边再做 2×2 平均，边缘比直接 1:1 干净
//   3) 转成 JPEG（质量 0.9），一张大约 25~40KB
// 原图动辄几 MB，在 ESP32 的 WiFi 上传要十几秒还容易断；30KB 是瞬间的事。
var TW=210, TH=248, UW=420, UH=496;

function prepare(f){
  return new Promise(function(res,rej){
    var url=URL.createObjectURL(f), img=new Image();
    img.onload=function(){
      URL.revokeObjectURL(url);
      try{
        var c=document.createElement('canvas');
        c.width=UW;c.height=UH;
        var g=c.getContext('2d');
        // 透明 PNG 不铺白底，转 JPEG 后透明区会变纯黑，1-bit 屏上就是一块黑斑
        g.fillStyle='#fff';g.fillRect(0,0,UW,UH);
        var sw=img.naturalWidth,sh=img.naturalHeight,r=TW/TH;
        var cw=sw,ch=Math.round(sw/r);
        if(ch>sh){ch=sh;cw=Math.round(sh*r);}
        g.imageSmoothingEnabled=true;
        g.imageSmoothingQuality='high';
        g.drawImage(img,Math.round((sw-cw)/2),Math.round((sh-ch)/2),cw,ch,0,0,UW,UH);
        c.toBlob(function(b){b?res(b):rej('图片转换失败')},'image/jpeg',0.9);
      }catch(e){rej('图片转换失败')}
    };
    img.onerror=function(){URL.revokeObjectURL(url);rej('不是有效的图片')};
    img.src=url;
  });
}

function up(files){
  if(!files||!files.length)return;
  // 只保留最后一张：这个模式就是"一张覆盖一张"
  var f=files[files.length-1];
  bar.style.display='block';barin.style.width='0';
  log.textContent='正在处理：'+f.name;
  prepare(f).then(function(b){
    log.textContent='正在上传（'+Math.round(b.size/1024)+' KB）';
    return post(b);
  }).then(function(){
    log.textContent='已显示到屏幕上';
    bar.style.display='none';file.value='';refresh();
  }).catch(function(err){
    log.textContent='失败：'+err;
    bar.style.display='none';file.value='';
  });
}
function post(b){
  return new Promise(function(res,rej){
    var x=new XMLHttpRequest();
    x.open('POST','/upload');
    x.upload.onprogress=function(e){if(e.lengthComputable)barin.style.width=(e.loaded/e.total*100)+'%'};
    x.onload=function(){x.status==200?res():rej(x.responseText||('HTTP '+x.status))};
    x.onerror=function(){rej('网络错误')};
    x.send(b);
  });
}
function refresh(){
  fetch('/list').then(function(r){return r.json()}).then(function(o){
    if(o.override){cur.textContent='网页上传的图片（重启后恢复默认）'}
    else if(o.sd_count>0){cur.textContent='SD 卡里的 '+o.sd_count+' 张照片'}
    else{cur.textContent='内置默认壁纸'}
  }).catch(function(){cur.textContent='读取失败'});
}
refresh();
</script>
</body></html>)HTML";

// ---------------------------------------------------------------- 工具函数

static std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size() &&
            isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            const char hex[3] = {in[i + 1], in[i + 2], 0};
            out.push_back(static_cast<char>(strtol(hex, nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// 只保留安全字符：字母数字、下划线、短横线、点。
// 中文等非 ASCII 字符会被替换成 '_'，既避免 FAT 长文件名踩坑，
// 也顺手挡掉 "../" 之类的路径穿越。
static std::string SanitizeName(const std::string& raw) {
    std::string base = raw;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }

    std::string out;
    out.reserve(base.size());
    for (char c : base) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (isalnum(u) || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }

    // 去掉开头的点，避免隐藏文件 / ".."
    while (!out.empty() && out.front() == '.') {
        out.erase(out.begin());
    }
    if (out.empty()) {
        out = "photo.jpg";
    }
    return out;
}

static bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static bool GetNameParam(httpd_req_t* req, std::string* out) {
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[192] = {0};
    if (httpd_query_key_value(query, "name", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    *out = SanitizeName(UrlDecode(raw));
    return !out->empty();
}

// ---------------------------------------------------------------- 路由处理

PhotoWebServer& PhotoWebServer::GetInstance() {
    static PhotoWebServer instance;
    return instance;
}

PhotoWebServer::~PhotoWebServer() {
    Stop();
}

bool PhotoWebServer::GetSafeName(httpd_req_t* req, char* out, size_t out_len) {
    std::string name;
    if (!GetNameParam(req, &name)) {
        return false;
    }
    if (!EndsWith(name, ".jpg") && !EndsWith(name, ".jpeg") && !EndsWith(name, ".JPG") &&
        !EndsWith(name, ".JPEG")) {
        return false;
    }
    snprintf(out, out_len, "%s", name.c_str());
    return true;
}

esp_err_t PhotoWebServer::IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoWebServer::ListHandler(httpd_req_t* req) {
    const bool override_active = HomePhoto::GetInstance().HasOverride();

    int sd_count = 0;
    auto& sd = SdcardManager::getInstance();
    if (sd.isMounted()) {
        auto files = sd.listFiles(kPhotoDir, ".jpg");
        auto files2 = sd.listFiles(kPhotoDir, ".jpeg");
        sd_count = static_cast<int>(files.size() + files2.size());
    }

    char json[96];
    snprintf(json, sizeof(json), "{\"override\":%s,\"sd_count\":%d}",
             override_active ? "true" : "false", sd_count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoWebServer::UploadHandler(httpd_req_t* req) {
    // 一次性图片：整张 JPEG 先读进 PSRAM，然后在内存里解码 + 抖动 + 直接显示。
    // 不写 SD 卡，所以插不插卡都能用，也不会"越传越多"——新的一张直接覆盖上一张。
    size_t cap = 256 * 1024;
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "内存不足，无法接收图片", HTTPD_RESP_USE_STRLEN);
    }

    char chunk[4096];
    size_t total = 0;
    bool failed = false;
    const char* fail_reason = nullptr;

    while (true) {
        const int r = httpd_req_recv(req, chunk, sizeof(chunk));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;  // 网络抖动，继续等
        }
        if (r <= 0) {
            break;
        }
        if (total + static_cast<size_t>(r) > kMaxUploadBytes) {
            failed = true;
            fail_reason = "图片超过 8MB";
            break;
        }
        if (total + static_cast<size_t>(r) > cap) {
            size_t new_cap = cap;
            while (new_cap < total + static_cast<size_t>(r)) {
                new_cap *= 2;
            }
            auto* grown = static_cast<uint8_t*>(heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM));
            if (grown == nullptr) {
                failed = true;
                fail_reason = "内存不足，图片太大";
                break;
            }
            buf = grown;
            cap = new_cap;
        }
        memcpy(buf + total, chunk, static_cast<size_t>(r));
        total += static_cast<size_t>(r);
    }

    if (!failed && total == 0) {
        failed = true;
        fail_reason = "空文件";
    }

    bool applied = false;
    if (!failed) {
        // 目标尺寸和主页照片位一致（外框 214×252 去掉 2px 边框）
        applied = HomePhoto::GetInstance().ApplyJpegMemory(buf, total, 210, 248);
    }
    heap_caps_free(buf);

    if (failed) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, fail_reason, HTTPD_RESP_USE_STRLEN);
    }
    if (!applied) {
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        return httpd_resp_send(req, "这张图解不开，请换一张 JPG/PNG", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "收到图片（%u 字节），已在内存中解码并显示", static_cast<unsigned>(total));
    GetInstance().NotifyUploaded("ram:upload");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoWebServer::DeleteHandler(httpd_req_t* req) {
    char name[192] = {0};
    if (!GetSafeName(req, name, sizeof(name))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "文件名无效", HTTPD_RESP_USE_STRLEN);
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", kPhotoDir, name);
    if (unlink(path) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "文件不存在", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "删除图片 %s", path);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------- 生命周期

bool PhotoWebServer::Start() {
    if (server_ != nullptr) {
        return true;
    }

    // 不再要求插 SD 卡：一次性图片走内存，没卡也能传
    if (SdcardManager::getInstance().isMounted()) {
        mkdir(kPhotoDir, 0777);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.recv_wait_timeout = 30;   // 大图上传给足 30 秒
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;  // 空闲连接自动回收，避免占满 socket

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t uris[] = {
        {"/",        HTTP_GET,  IndexHandler,  nullptr},
        {"/list",    HTTP_GET,  ListHandler,   nullptr},
        {"/upload",  HTTP_POST, UploadHandler, nullptr},
        {"/delete",  HTTP_POST, DeleteHandler, nullptr},
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server_, &uri);
    }

    ESP_LOGI(TAG, "图片上传服务已启动，地址: %s", GetUrl().c_str());
    return true;
}

void PhotoWebServer::Stop() {
    if (server_ == nullptr) {
        return;
    }
    httpd_stop(server_);
    server_ = nullptr;
    ESP_LOGI(TAG, "图片上传服务已停止");
}

std::string PhotoWebServer::GetUrl() const {
    const std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) {
        return "";
    }
    return "http://" + ip;
}
