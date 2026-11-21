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
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../serial_console.h"
#include "avatar_loader.h"
#include "board.h"
#include "display.h"
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
#drop,#adrop{border:2px dashed #c7c7cc;border-radius:12px;padding:22px 12px;text-align:center;color:#86868b;font-size:14px;cursor:pointer;transition:.2s}
#drop.on,#adrop.on{border-color:#0071e3;background:#f0f7ff;color:#0071e3}
#file,#afile{display:none}
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
<div class="sub">选一张就行，传完立刻显示在屏幕上，并且会一直保留——关机、重启、拔电都还是这张。</div>
<div class="card">
  <div id="drop">点击选择图片<br><span style="font-size:12px">也可以把图片拖进来</span></div>
  <input type="file" id="file" accept="image/*">
  <div class="bar" id="bar"><i id="barin"></i></div>
  <div id="log"></div>
</div>
<div class="card">
  <div style="font-size:14px;font-weight:600;margin-bottom:6px">屏幕当前显示</div>
  <div id="cur" style="font-size:13px;color:#86868b">读取中…</div>
  <button id="reset" style="margin-top:12px;width:100%;padding:10px;border:1px solid #e5e5ea;background:#fff;border-radius:10px;font-size:14px;color:#1d1d1f;cursor:pointer">恢复内置壁纸</button>
</div>
<div class="card">
  <div style="font-size:14px;font-weight:600;margin-bottom:6px">播放器头像（唱片中心）</div>
  <div class="sub" style="margin-bottom:10px">建议正方形图片，会自动裁成 52×52 黑白点阵。传完进播放器页就能看到。</div>
  <div id="adrop">点击选择头像<br><span style="font-size:12px">也可以把图片拖进来</span></div>
  <input type="file" id="afile" accept="image/*">
  <div id="alog" style="margin-top:10px;font-size:13px;color:#86868b;min-height:18px"></div>
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

// ===== 播放器头像（唱片中心，52×52）=====
// 不用前端裁剪：固件那边会居中裁成 52×52 再做抖动，随便传张方图就行。
var adrop=document.getElementById('adrop'),afile=document.getElementById('afile'),
    alog=document.getElementById('alog');
adrop.onclick=function(){afile.click()};
adrop.ondragover=function(e){e.preventDefault();adrop.classList.add('on')};
adrop.ondragleave=function(){adrop.classList.remove('on')};
adrop.ondrop=function(e){e.preventDefault();adrop.classList.remove('on');
  if(e.dataTransfer.files.length)upAvatar(e.dataTransfer.files[0])};
afile.onchange=function(){if(afile.files.length)upAvatar(afile.files[0])};

// 头像先在浏览器里裁成 52×52 并转成标准 JPEG 再传。
// 不这么做的话，手机相册里的 HEIC / 带透明通道的 PNG / 渐进式 JPEG
// 板子都解不开（报 "不是 JPEG 或已损坏"）。统一转一遍，格式就齐了。
// 用 cover 裁剪（短边铺满、居中裁掉多余），和屏幕上的观感一致。
function prepareAvatar(f){
  return new Promise(function(res,rej){
    var url=URL.createObjectURL(f), img=new Image();
    img.onload=function(){
      URL.revokeObjectURL(url);
      try{
        var S=52, c=document.createElement('canvas');
        c.width=S;c.height=S;
        var g=c.getContext('2d');
        g.fillStyle='#fff';g.fillRect(0,0,S,S);   // 透明区铺白，避免转 JPEG 后变黑块
        var sw=img.naturalWidth,sh=img.naturalHeight;
        var s=Math.max(S/sw,S/sh);                // cover
        var dw=Math.round(sw*s),dh=Math.round(sh*s);
        g.imageSmoothingEnabled=true;
        g.imageSmoothingQuality='high';
        g.drawImage(img,(S-dw)/2,(S-dh)/2,dw,dh);
        c.toBlob(function(b){ b?res(b):rej(new Error('图片编码失败')); },'image/jpeg',0.95);
      }catch(e){rej(e);}
    };
    img.onerror=function(){URL.revokeObjectURL(url);rej(new Error('浏览器读不了这张图，换 JPG/PNG 试试'))};
    img.src=url;
  });
}

function upAvatar(f){
  if(!f) return;
  alog.textContent='正在处理图片…';
  prepareAvatar(f).then(function(blob){
    alog.textContent='上传中…';
    var x=new XMLHttpRequest();
    x.open('POST','/avatar');
    x.timeout=20000;
    x.onload=function(){
      if(x.status===200){alog.textContent='头像已更新，进播放器页看看';}
      else{alog.textContent='失败：'+(x.responseText||x.status);}
    };
    x.onerror=function(){alog.textContent='上传失败，确认手机和板子在同一 WiFi'};
    x.ontimeout=function(){alog.textContent='上传超时，重试一次'};
    x.send(blob);
  }).catch(function(e){
    alog.textContent='处理失败：'+e.message;
  });
}
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
    if(o.override){cur.textContent='你上传的图片（已保存，重启后仍是这张）'}
    else if(o.sd_count>0){cur.textContent='SD 卡里的 '+o.sd_count+' 张照片'}
    else{cur.textContent='内置默认壁纸'}
    document.getElementById('reset').style.display=o.override?'block':'none';
  }).catch(function(){cur.textContent='读取失败'});
}
document.getElementById('reset').onclick=function(){
  if(!confirm('恢复成内置默认壁纸？\n（存着的那张会一起删掉）'))return;
  this.disabled=true;
  var self=this;
  fetch('/reset',{method:'POST'}).then(function(r){
    log.textContent = r.ok ? '已恢复内置壁纸' : '恢复失败（HTTP '+r.status+'）';
    self.disabled=false;refresh();
  }).catch(function(e){log.textContent='恢复失败：'+e;self.disabled=false});
};
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

// ===== WiFi 省电 ⇄ 网页速度 =====
// 设备空闲时 application 会切到 LOW_POWER(MAX_MODEM)，DTIM/listen interval 拉到
// 1.02 秒，射频每秒才醒一次。浏览器打开页面要串行发好几个请求，每个都白等一轮，
// 五六个 KB 的页面就要四五秒，手机端直接判超时——这就是"网站打不开"。
// 做法：只要有 HTTP 请求进来就把 WiFi 拉到 PERFORMANCE，后台任务等闲置 15 秒再收回。
void PhotoWebServer::MarkActive() {
    auto& self = GetInstance();
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    self.last_active_ms_.store(now);

    // 只在"从省电刚醒"时切一次：esp_wifi_set_ps 本身要下发命令，
    // 每个请求都调一遍纯属浪费。
    if (!self.wifi_boosted_.load()) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        self.wifi_boosted_.store(true);
        ESP_LOGI(TAG, "有网页在访问，WiFi 临时切到高性能（省电模式下每个请求要等约 1 秒）");
    }
}

uint32_t PhotoWebServer::MsSinceActive() const {
    const uint32_t last = last_active_ms_.load();
    if (last == kNeverActive) {
        return kNeverActive;
    }
    // esp_timer 是 64 位微秒，只取低 32 位做差，回绕（约 71 分钟）也能算对
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    return now - last;
}

void PhotoWebServer::ReleaseWifiBoost() {
    if (!wifi_boosted_.exchange(false)) {
        return;
    }
    last_active_ms_.store(kNeverActive);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(TAG, "网页闲置，WiFi 回到省电模式");
}

esp_err_t PhotoWebServer::IndexHandler(httpd_req_t* req) {
    MarkActive();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoWebServer::ListHandler(httpd_req_t* req) {
    MarkActive();
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

// 恢复内置壁纸：把 NVS 里那份常驻壁纸连同内存里的 override 一起删掉。
// 删完立刻通知 UI 重新挑一张（SD 卡有图就显示 SD 的，没有就内置图）。
esp_err_t PhotoWebServer::ResetHandler(httpd_req_t* req) {
    MarkActive();
    HomePhoto::GetInstance().ForgetSavedOverride();
    GetInstance().NotifyUploaded("ram:reset");

    ESP_LOGI(TAG, "已恢复内置壁纸");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoWebServer::UploadHandler(httpd_req_t* req) {
    // 一进上传就拉高性能：上传是几十上百 KB 的连续传输，
    // 省电模式下每个空口机会都要等一轮 DTIM，能传上十几秒。
    MarkActive();
    // 一次性图片：整张 JPEG 先读进 PSRAM，然后在内存里解码 + 抖动 + 直接显示。
    // 不写 SD 卡，所以插不插卡都能用，也不会"越传越多"——新的一张直接覆盖上一张。
    //
    // 接收缓冲和图片缓冲都放 PSRAM：httpd 任务栈只有 8KB，以前把 4KB 的 chunk 放在
    // 栈上、再往下走 JPEG 解码（esp_jpeg 内部还要一两 KB），栈直接溢出，
    // 表现就是"传到一半连接断了 / 一直传不上去"。
    constexpr int kChunkSize = 4096;
    size_t cap = 256 * 1024;
    auto* chunk = static_cast<char*>(heap_caps_malloc(kChunkSize, MALLOC_CAP_SPIRAM));
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (chunk == nullptr || buf == nullptr) {
        heap_caps_free(chunk);
        heap_caps_free(buf);
        ESP_LOGE(TAG, "上传缓冲分配失败（内部剩余 %u / PSRAM 剩余 %u）",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "内存不足，无法接收图片", HTTPD_RESP_USE_STRLEN);
    }

    const int content_len = static_cast<int>(req->content_len);
    ESP_LOGI(TAG, "开始接收上传：Content-Length=%d", content_len);

    size_t total = 0;
    bool failed = false;
    const char* fail_reason = nullptr;
    const char* fail_status = "500 Internal Server Error";

    while (true) {
        const int r = httpd_req_recv(req, chunk, kChunkSize);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;  // 网络抖动，继续等
        }
        if (r <= 0) {
            break;
        }
        if (total + static_cast<size_t>(r) > kMaxUploadBytes) {
            failed = true;
            fail_status = "413 Payload Too Large";
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
                fail_status = "413 Payload Too Large";
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
        fail_status = "400 Bad Request";
        fail_reason = "空文件";
    }
    // Content-Length 有值却没收全：多半是手机端切后台/断了，重传即可
    if (!failed && content_len > 0 && total != static_cast<size_t>(content_len)) {
        failed = true;
        fail_status = "400 Bad Request";
        fail_reason = "图片没有传完，请重试";
    }

    bool applied = false;
    if (!failed) {
        // 按 URI 决定目标：/avatar 是播放器页的唱片中心头像，其余是主页壁纸。
        // 头像走独立的 AvatarLoader（52×52，另存一个 NVS key），
        // 和壁纸互不干扰，各自都能单独换。
        if (strcmp(req->uri, "/avatar") == 0) {
            applied = AvatarLoader::GetInstance().ApplyJpeg(buf, total, "网页上传");
            if (applied) {
                // 立刻刷新播放器页中心（若当前不在该页，下次进入时也会读到新图）
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    display->SetEmotion("neutral");
                }
            }
        } else {
            // 目标尺寸和主页照片位一致（外框 214×252 去掉 2px 边框）
            applied = HomePhoto::GetInstance().ApplyJpegMemory(buf, total, 210, 248);
        }
    }
    heap_caps_free(buf);
    heap_caps_free(chunk);

    if (failed) {
        ESP_LOGW(TAG, "上传失败：%s（收到 %u 字节）", fail_reason, static_cast<unsigned>(total));
        httpd_resp_set_status(req, fail_status);
        return httpd_resp_send(req, fail_reason, HTTPD_RESP_USE_STRLEN);
    }
    if (!applied) {
        ESP_LOGW(TAG, "上传的图解码失败（%u 字节，不是 JPEG 或已损坏）", static_cast<unsigned>(total));
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        return httpd_resp_send(req, "这张图解不开，请换一张 JPG/PNG", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "收到图片（%u 字节），已在内存中解码并显示", static_cast<unsigned>(total));
    GetInstance().NotifyUploaded("ram:upload");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoWebServer::DeleteHandler(httpd_req_t* req) {
    MarkActive();
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

// ===== 无线命令通道 =====
// GET /cmd?c=status
// 把串口那套命令原样搬到 WiFi 上：不插 USB 线也能控制板子。
// 电脑端（Java 中转服务或浏览器）拿到板子 IP 后直接请求这个接口就行。
//
// 为什么 play 要另开任务：搜歌 + 取直链是同步 HTTP，要几秒到十几秒，
// 在 httpd 任务里干会把请求拖到超时（而且 httpd 栈只有 8KB，跑不下 TLS）。

static void UrlDecodeInPlace(char* s) {
    char* dst = s;
    while (*s != '\0') {
        if (*s == '+') {
            *dst++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit(static_cast<unsigned char>(s[1])) &&
                   isxdigit(static_cast<unsigned char>(s[2]))) {
            char hex[3] = {s[1], s[2], '\0'};
            *dst++ = static_cast<char>(strtol(hex, nullptr, 16));
            s += 3;
        } else {
            *dst++ = *s++;
        }
    }
    *dst = '\0';
}

static void RemoteCmdTask(void* arg) {
    char* text = static_cast<char*>(arg);
    if (text != nullptr) {
        SerialConsole::GetInstance().Execute(text);
        free(text);
    }
    vTaskDelete(nullptr);
}

esp_err_t PhotoWebServer::CmdHandler(httpd_req_t* req) {
    GetInstance().MarkActive();

    char query[384];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "缺少参数 c", HTTPD_RESP_USE_STRLEN);
    }

    char cmd[256];
    if (httpd_query_key_value(query, "c", cmd, sizeof(cmd)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "缺少参数 c", HTTPD_RESP_USE_STRLEN);
    }
    UrlDecodeInPlace(cmd);

    // 点歌是慢操作，丢给独立任务，立刻回话，不然浏览器/电脑端会等到超时
    if (strncmp(cmd, "play", 4) == 0) {
        char* copy = strdup(cmd);
        if (copy == nullptr) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "内存不足", HTTPD_RESP_USE_STRLEN);
        }
        // 栈放内部 RAM：搜索路径里有 NVS 读、TLS，放 PSRAM 容易踩 cache 关闭的坑
        if (xTaskCreate(RemoteCmdTask, "remote_cmd", 12288, copy, 4, nullptr) != pdPASS) {
            free(copy);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "任务创建失败", HTTPD_RESP_USE_STRLEN);
        }
        ESP_LOGI(TAG, "远程点歌: %s", cmd);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "OK 已开始搜索，稍等几秒；结果看屏幕或听声音", HTTPD_RESP_USE_STRLEN);
    }

    const std::string result = SerialConsole::GetInstance().Execute(cmd);
    ESP_LOGI(TAG, "远程命令: %s", cmd);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, result.c_str(), result.size());
}

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
    // 栈要够 JPEG 解码用：handler 里已经不放大数组了，但 esp_jpeg 内部还有
    // Huffman 表 / 行缓冲，6KB 是紧的，给 8KB 留余量（内部 SRAM 现在有余）。
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;   // 大图上传给足 30 秒
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;  // 空闲连接自动回收，避免占满 socket

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（内部 SRAM 剩余 %u 字节）",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t uris[] = {
        {"/",        HTTP_GET,  IndexHandler,  nullptr},
        {"/list",    HTTP_GET,  ListHandler,   nullptr},
        {"/upload",  HTTP_POST, UploadHandler, nullptr},
        {"/avatar",  HTTP_POST, UploadHandler, nullptr},   // 同一个 handler，按 URI 分流
        {"/delete",  HTTP_POST, DeleteHandler, nullptr},
        {"/reset",   HTTP_POST, ResetHandler,  nullptr},
        {"/cmd",     HTTP_GET,  CmdHandler,    nullptr},
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server_, &uri);
    }

    // 新起的服务，还没有任何人访问过
    last_active_ms_.store(kNeverActive);
    wifi_boosted_.store(false);

    ESP_LOGI(TAG, "图片上传服务已启动，地址: %s", GetUrl().c_str());
    return true;
}

void PhotoWebServer::Stop() {
    if (server_ == nullptr) {
        return;
    }
    httpd_stop(server_);
    server_ = nullptr;
    ReleaseWifiBoost();   // 服务没了就别再占着高性能
    ESP_LOGI(TAG, "图片上传服务已停止");
}

std::string PhotoWebServer::GetUrl() const {
    const std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) {
        return "";
    }
    return "http://" + ip;
}
