#include "photo_web_server.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>

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
<h1>相册上传</h1>
<div class="sub">图片存到设备 SD 卡，主页每天自动换一张</div>
<div class="card">
  <div id="drop">点击选择图片<br><span style="font-size:12px">也可以把图片拖进来</span></div>
  <input type="file" id="file" accept="image/*" multiple>
  <label style="display:flex;align-items:center;gap:8px;margin-top:12px;font-size:12.5px;color:#86868b;cursor:pointer">
    <input type="checkbox" id="raw" style="margin:0">保留原图（不裁剪不压缩，占 SD 空间、上传慢）
  </label>
  <div class="bar" id="bar"><i id="barin"></i></div>
  <div id="log"></div>
</div>
<div class="card">
  <div style="font-size:14px;font-weight:600;margin-bottom:4px">设备上的图片</div>
  <ul id="list"><li class="empty">加载中</li></ul>
</div>
<script>
var drop=document.getElementById('drop'),file=document.getElementById('file'),
    bar=document.getElementById('bar'),barin=document.getElementById('barin'),
    log=document.getElementById('log');
drop.onclick=function(){file.click()};
drop.ondragover=function(e){e.preventDefault();drop.classList.add('on')};
drop.ondragleave=function(){drop.classList.remove('on')};
drop.ondrop=function(e){e.preventDefault();drop.classList.remove('on');up(e.dataTransfer.files)};
file.onchange=function(){up(file.files)};
// 屏幕上的照片位只有 210×248，还是黑白点阵。所以上传前先在浏览器里裁好缩好再发：
//   1) 按 210:248 居中裁剪（和固件里的 cover 裁剪同一套规则，网页上看到的就是屏上的构图）
//   2) 缩到 420×496（2 倍），固件那边再做 2×2 平均，边缘比直接 1:1 干净
//   3) 转成 JPEG（质量 0.9），一张大约 25~40KB
// 8MB 原图在 ESP32 的 WiFi 上要传十几秒还容易断，还会把 SD 卡塞满；30KB 是瞬间的事。
// 勾上"保留原图"则原封不动上传（用于别的用途）。
var TW=210, TH=248, UW=420, UH=496;

function jpgName(n){
  var d=n.lastIndexOf('.');
  return ((d<0?n:n.substring(0,d))||'photo')+'.jpg';
}

// 裁成 210:248 再缩到 420×496，返回 JPEG Blob
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
  bar.style.display='block';barin.style.width='0';
  var raw=document.getElementById('raw').checked;
  var i=0,ok=0;
  function next(){
    if(i>=files.length){
      log.textContent='完成，成功 '+ok+'/'+files.length;
      bar.style.display='none';file.value='';loadList();return;
    }
    var f=files[i];
    log.textContent='正在处理 '+(i+1)+'/'+files.length+'：'+f.name;
    (raw?Promise.resolve(f):prepare(f)).then(function(b){
      log.textContent='正在上传 '+(i+1)+'/'+files.length+'：'+f.name+
                      '（'+Math.round(b.size/1024)+' KB）';
      return post(b, raw?f.name:jpgName(f.name));
    }).then(function(){ok++;i++;next()},function(err){
      log.textContent=f.name+' 失败：'+err;i++;next();
    });
  }
  next();
}
function post(b,name){
  return new Promise(function(res,rej){
    var x=new XMLHttpRequest();
    x.open('POST','/upload?name='+encodeURIComponent(name));
    x.upload.onprogress=function(e){if(e.lengthComputable)barin.style.width=(e.loaded/e.total*100)+'%'};
    x.onload=function(){x.status==200?res():rej(x.responseText||('HTTP '+x.status))};
    x.onerror=function(){rej('网络错误')};
    x.send(b);
  });
}
function esc(s){return s.replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
function loadList(){
  var ul=document.getElementById('list');
  fetch('/list').then(function(r){return r.json()}).then(function(arr){
    if(!arr.length){ul.innerHTML='<li class="empty">还没有图片</li>';return}
    ul.innerHTML=arr.map(function(n){
      return '<li><span>'+esc(n)+'</span><button class="del" data-n="'+esc(n)+'">删除</button></li>';
    }).join('');
    var ds=ul.querySelectorAll('.del');
    for(var k=0;k<ds.length;k++){
      ds[k].onclick=function(){
        var n=this.getAttribute('data-n');
        if(!confirm('删除 '+n+' ?'))return;
        fetch('/delete?name='+encodeURIComponent(n),{method:'POST'}).then(loadList);
      };
    }
  }).catch(function(){ul.innerHTML='<li class="empty">读取失败</li>'});
}
loadList();
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

// 文件名冲突时追加 _1 _2 ...
static std::string UniquePath(const std::string& dir, const std::string& name) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%s", dir.c_str(), name.c_str());
    if (access(buf, F_OK) != 0) {
        return buf;
    }

    const size_t dot = name.find_last_of('.');
    const std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
    const std::string ext = (dot == std::string::npos) ? "" : name.substr(dot);

    for (int i = 1; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "%s/%s_%d%s", dir.c_str(), stem.c_str(), i, ext.c_str());
        if (access(buf, F_OK) != 0) {
            return buf;
        }
    }
    return buf;
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
    std::string json = "[";
    bool first = true;

    auto& sd = SdcardManager::getInstance();
    if (sd.isMounted()) {
        auto files = sd.listFiles(kPhotoDir, ".jpg");
        auto files2 = sd.listFiles(kPhotoDir, ".jpeg");
        files.insert(files.end(), files2.begin(), files2.end());

        for (const auto& path : files) {
            const size_t slash = path.find_last_of('/');
            const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
            if (!first) {
                json += ",";
            }
            json += "\"" + base + "\"";
            first = false;
        }
    }
    json += "]";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t PhotoWebServer::UploadHandler(httpd_req_t* req) {
    char name[192] = {0};
    if (!GetSafeName(req, name, sizeof(name))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "文件名无效，仅接受 .jpg / .jpeg", HTTPD_RESP_USE_STRLEN);
    }

    auto& sd = SdcardManager::getInstance();
    if (!sd.isMounted()) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "SD 卡未挂载", HTTPD_RESP_USE_STRLEN);
    }

    mkdir(kPhotoDir, 0777);

    const std::string path = UniquePath(kPhotoDir, name);
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "打不开文件写入: %s", path.c_str());
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "SD 卡写入失败", HTTPD_RESP_USE_STRLEN);
    }

    char buf[4096];
    size_t total = 0;
    bool too_big = false;
    while (true) {
        const int r = httpd_req_recv(req, buf, sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;  // 网络抖动，继续等
        }
        if (r <= 0) {
            break;
        }
        if (total + static_cast<size_t>(r) > kMaxUploadBytes) {
            too_big = true;
            break;
        }
        if (fwrite(buf, 1, static_cast<size_t>(r), f) != static_cast<size_t>(r)) {
            fclose(f);
            unlink(path.c_str());
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "写入中断", HTTPD_RESP_USE_STRLEN);
        }
        total += static_cast<size_t>(r);
    }
    fclose(f);

    if (too_big || total == 0) {
        unlink(path.c_str());
        httpd_resp_set_status(req, too_big ? "413 Payload Too Large" : "400 Bad Request");
        return httpd_resp_send(req, too_big ? "图片超过 8MB" : "空文件", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "收到图片 %s（%u 字节）", path.c_str(), static_cast<unsigned>(total));
    GetInstance().NotifyUploaded(path);

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

    if (!SdcardManager::getInstance().isMounted()) {
        ESP_LOGE(TAG, "SD 卡未挂载，无法提供图片上传");
        return false;
    }
    mkdir(kPhotoDir, 0777);

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
