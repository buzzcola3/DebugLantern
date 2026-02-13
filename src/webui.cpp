#include "webui.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace debuglantern {

namespace {

// ---------------------------------------------------------------------------
// Embedded HTML dashboard
// ---------------------------------------------------------------------------
static const char *kHtmlPage = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>debuglantern</title>
<style>
:root{--bg:#0f0f23;--card:#1a1a2e;--accent:#e94560;--green:#4ecca3;--yellow:#ffc107;--gray:#666;--text:#e0e0e0;--border:#2a2a4a}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'SF Mono','Fira Code','Cascadia Code',monospace;background:var(--bg);color:var(--text);min-height:100vh}
.container{max-width:1100px;margin:0 auto;padding:24px}
header{display:flex;align-items:center;gap:12px;margin-bottom:24px}
header h1{font-size:1.4rem;color:var(--accent)}
header .icon{font-size:2rem}
.status-bar{display:flex;gap:16px;margin-bottom:24px;font-size:.8rem;color:var(--gray)}
.status-bar .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px;background:var(--green);vertical-align:middle}
.status-bar .dot.off{background:var(--accent)}
.upload-zone{border:2px dashed var(--border);border-radius:12px;padding:32px;text-align:center;margin-bottom:24px;transition:all .2s;cursor:pointer}
.upload-zone:hover,.upload-zone.dragover{border-color:var(--accent);background:rgba(233,69,96,.05)}
.upload-zone input{display:none}
.upload-zone p{color:var(--gray);font-size:.9rem}
table{width:100%;border-collapse:collapse}
th,td{padding:10px 12px;text-align:left;border-bottom:1px solid var(--border)}
th{color:var(--gray);font-size:.75rem;text-transform:uppercase;letter-spacing:1px}
.badge{display:inline-block;padding:2px 10px;border-radius:12px;font-size:.75rem;font-weight:600}
.badge-loaded{background:var(--yellow);color:#000}
.badge-running{background:var(--green);color:#000}
.badge-debugging{background:var(--accent);color:#fff}
.badge-stopped{background:var(--gray);color:#fff}
.id-cell{font-size:.8rem;color:var(--gray);cursor:pointer}
.id-cell:hover{color:var(--text)}
.actions button{background:var(--card);border:1px solid var(--border);color:var(--text);padding:4px 10px;border-radius:6px;cursor:pointer;font-size:.75rem;margin-right:4px;font-family:inherit;transition:border-color .15s}
.actions button:hover{border-color:var(--accent)}
.actions button.danger:hover{border-color:#ff4444}
.empty{text-align:center;color:var(--gray);padding:48px;font-size:.9rem}
.toast-container{position:fixed;bottom:20px;right:20px;z-index:1000;display:flex;flex-direction:column-reverse;gap:8px}
.toast{background:var(--card);border:1px solid var(--border);padding:10px 18px;border-radius:8px;font-size:.8rem;animation:fadeIn .25s ease}
.toast.error{border-color:var(--accent)}
@keyframes fadeIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
</style>
</head>
<body>
<div class="container">
  <header>
    <span class="icon">&#x1F3EE;</span>
    <h1>debuglantern</h1>
  </header>
  <div class="status-bar">
    <span><span class="dot" id="conn-dot"></span><span id="conn-text">connected</span></span>
    <span id="session-count">0 sessions</span>
  </div>
  <div class="upload-zone" id="upload-zone" onclick="document.getElementById('file-input').click()">
    <input type="file" id="file-input">
    <p>Drop ELF binary here or click to upload</p>
  </div>
  <table id="table" style="display:none">
    <thead>
      <tr><th>ID</th><th>State</th><th>PID</th><th>Debug Port</th><th>Actions</th></tr>
    </thead>
    <tbody id="sessions"></tbody>
  </table>
  <div class="empty" id="empty">No sessions</div>
</div>
<div class="toast-container" id="toasts"></div>
<script>
const $=s=>document.getElementById(s);
let connected=false;

function toast(msg,err){
  const el=document.createElement('div');
  el.className='toast'+(err?' error':'');
  el.textContent=msg;
  $('toasts').appendChild(el);
  setTimeout(()=>el.remove(),3500);
}

function badge(state){
  return '<span class="badge badge-'+state.toLowerCase()+'">'+state+'</span>';
}

function actions(s){
  let h='';
  if(s.state==='LOADED'||s.state==='STOPPED'){
    h+='<button onclick="act(\'start\',\''+s.id+'\')">Start</button>';
    h+='<button onclick="act(\'start\',\''+s.id+'\',true)">Debug Start</button>';
    h+='<button class="danger" onclick="act(\'delete\',\''+s.id+'\')">Delete</button>';
  }
  if(s.state==='RUNNING'){
    h+='<button onclick="act(\'debug\',\''+s.id+'\')">Attach GDB</button>';
    h+='<button onclick="act(\'stop\',\''+s.id+'\')">Stop</button>';
    h+='<button class="danger" onclick="act(\'kill\',\''+s.id+'\')">Kill</button>';
  }
  if(s.state==='DEBUGGING'){
    h+='<button onclick="act(\'stop\',\''+s.id+'\')">Stop</button>';
    h+='<button class="danger" onclick="act(\'kill\',\''+s.id+'\')">Kill</button>';
  }
  return h;
}

function render(sessions){
  const tb=$('sessions');
  $('table').style.display=sessions.length?'table':'none';
  $('empty').style.display=sessions.length?'none':'block';
  $('session-count').textContent=sessions.length+' session'+(sessions.length!==1?'s':'');
  tb.innerHTML=sessions.map(s=>'<tr>'+
    '<td class="id-cell" title="'+s.id+'" onclick="navigator.clipboard.writeText(\''+s.id+'\');toast(\'Copied ID\')">'+s.id.substring(0,8)+'&hellip;</td>'+
    '<td>'+badge(s.state)+'</td>'+
    '<td>'+(s.pid||'&mdash;')+'</td>'+
    '<td>'+(s.debug_port||'&mdash;')+'</td>'+
    '<td class="actions">'+actions(s)+'</td>'+
    '</tr>').join('');
}

async function refresh(){
  try{
    const r=await fetch('/api/sessions');
    const d=await r.json();
    render(d);
    setConnected(true);
  }catch(e){setConnected(false);}
}

function setConnected(v){
  connected=v;
  $('conn-dot').className='dot'+(v?'':' off');
  $('conn-text').textContent=v?'connected':'disconnected';
}

async function act(cmd,id,debug){
  try{
    const q=debug?'?flags=--debug':'';
    const r=await fetch('/api/sessions/'+id+'/'+cmd+q,{method:'POST'});
    const d=await r.json();
    if(d.error_code){toast(d.message,true);}
    else{toast(cmd+': '+d.state);}
    refresh();
  }catch(e){toast('Failed: '+e.message,true);}
}

async function upload(file){
  if(!file)return;
  toast('Uploading '+file.name+'...');
  try{
    const r=await fetch('/api/upload',{method:'POST',body:file,headers:{'Content-Type':'application/octet-stream'}});
    const d=await r.json();
    if(d.error_code){toast(d.message,true);}
    else{toast('Uploaded: '+d.id.substring(0,8));}
    refresh();
  }catch(e){toast('Upload failed',true);}
}

$('file-input').addEventListener('change',function(){upload(this.files[0]);this.value='';});
const zone=$('upload-zone');
zone.addEventListener('dragover',e=>{e.preventDefault();zone.classList.add('dragover');});
zone.addEventListener('dragleave',()=>zone.classList.remove('dragover'));
zone.addEventListener('drop',e=>{e.preventDefault();zone.classList.remove('dragover');if(e.dataTransfer.files.length)upload(e.dataTransfer.files[0]);});

let evtSrc;
function connectSSE(){
  evtSrc=new EventSource('/api/events');
  evtSrc.onmessage=e=>{try{render(JSON.parse(e.data));setConnected(true);}catch(err){}};
  evtSrc.onerror=()=>{evtSrc.close();setConnected(false);setTimeout(connectSSE,3000);};
}
connectSSE();
document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh();});
refresh();
</script>
</body>
</html>)HTML";

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    size_t content_length = 0;
};

bool read_request(int fd, HttpRequest &req) {
    std::string buf;
    char tmp[8192];

    while (buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > 65536) return false;
    }

    auto header_end = buf.find("\r\n\r\n");
    std::string headers = buf.substr(0, header_end);
    std::string rest = buf.substr(header_end + 4);

    // First line
    auto le = headers.find("\r\n");
    std::string first = headers.substr(0, le);
    auto sp1 = first.find(' ');
    auto sp2 = first.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = first.substr(0, sp1);
    std::string full_path = first.substr(sp1 + 1, sp2 - sp1 - 1);

    auto qp = full_path.find('?');
    if (qp != std::string::npos) {
        req.path = full_path.substr(0, qp);
        req.query = full_path.substr(qp + 1);
    } else {
        req.path = full_path;
    }

    // Content-Length (case-insensitive)
    std::string lh = headers;
    std::transform(lh.begin(), lh.end(), lh.begin(), ::tolower);
    auto cl = lh.find("content-length:");
    if (cl != std::string::npos) {
        auto vs = cl + 15;
        while (vs < lh.size() && lh[vs] == ' ') vs++;
        auto ve = lh.find("\r\n", vs);
        if (ve == std::string::npos) ve = lh.size();
        req.content_length = std::stoull(lh.substr(vs, ve - vs));
    }

    // Read body
    req.body = rest;
    while (req.body.size() < req.content_length) {
        ssize_t n = read(fd, tmp, std::min(sizeof(tmp), req.content_length - req.body.size()));
        if (n <= 0) return false;
        req.body.append(tmp, static_cast<size_t>(n));
    }

    return true;
}

std::vector<std::string> split_path(const std::string &path) {
    std::vector<std::string> parts;
    std::istringstream iss(path);
    std::string part;
    while (std::getline(iss, part, '/')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

void send_http(int fd, int status, const std::string &ctype, const std::string &body) {
    const char *text = "OK";
    if (status == 204) text = "No Content";
    else if (status == 400) text = "Bad Request";
    else if (status == 404) text = "Not Found";
    else if (status == 500) text = "Internal Server Error";

    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << text << "\r\n";
    if (!ctype.empty()) oss << "Content-Type: " << ctype << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string resp = oss.str();
    size_t off = 0;
    while (off < resp.size()) {
        ssize_t n = write(fd, resp.data() + off, resp.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

std::string trim_newlines(const std::string &s) {
    auto e = s.find_last_not_of("\r\n");
    return (e == std::string::npos) ? "" : s.substr(0, e + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// WebUI implementation
// ---------------------------------------------------------------------------

WebUI::WebUI(int web_port, int control_port)
    : web_port_(web_port), control_port_(control_port) {}

WebUI::~WebUI() { stop(); }

bool WebUI::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("webui: socket");
        return false;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(web_port_));

    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("webui: bind");
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 32) < 0) {
        perror("webui: listen");
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    thread_ = std::thread(&WebUI::run, this);
    return true;
}

void WebUI::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void WebUI::run() {
    while (running_) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
        if (fd < 0) continue;

        std::thread([this, fd]() {
            handle_client(fd);
            close(fd);
        }).detach();
    }
}

void WebUI::handle_client(int fd) {
    // Set socket timeouts
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    HttpRequest req;
    if (!read_request(fd, req)) return;

    auto parts = split_path(req.path);

    // CORS preflight
    if (req.method == "OPTIONS") {
        send_http(fd, 204, "", "");
        return;
    }

    // HTML page
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        send_http(fd, 200, "text/html; charset=utf-8", kHtmlPage);
        return;
    }

    // Favicon
    if (req.path == "/favicon.ico") {
        send_http(fd, 204, "", "");
        return;
    }

    // GET /api/sessions
    if (req.method == "GET" && parts.size() == 2 &&
        parts[0] == "api" && parts[1] == "sessions") {
        auto resp = proxy("LIST");
        send_http(fd, 200, "application/json", resp);
        return;
    }

    // GET /api/events (SSE)
    if (req.method == "GET" && parts.size() == 2 &&
        parts[0] == "api" && parts[1] == "events") {
        serve_sse(fd);
        return;
    }

    // POST /api/upload
    if (req.method == "POST" && parts.size() == 2 &&
        parts[0] == "api" && parts[1] == "upload") {
        auto resp = proxy_upload(req.body.data(), req.body.size());
        send_http(fd, 200, "application/json", resp);
        return;
    }

    // POST /api/sessions/{id}/{action}
    if (parts.size() == 4 && parts[0] == "api" && parts[1] == "sessions") {
        std::string id = parts[2];
        std::string action = parts[3];
        std::string cmd;

        if (action == "start") {
            cmd = "START " + id;
            if (req.query.find("debug") != std::string::npos) cmd += " --debug";
        } else if (action == "stop")   { cmd = "STOP " + id; }
          else if (action == "kill")   { cmd = "KILL " + id; }
          else if (action == "debug")  { cmd = "DEBUG " + id; }
          else if (action == "delete") { cmd = "DELETE " + id; }

        if (!cmd.empty()) {
            auto resp = proxy(cmd);
            send_http(fd, 200, "application/json", resp);
            return;
        }
    }

    // DELETE /api/sessions/{id}
    if (req.method == "DELETE" && parts.size() == 3 &&
        parts[0] == "api" && parts[1] == "sessions") {
        auto resp = proxy("DELETE " + parts[2]);
        send_http(fd, 200, "application/json", resp);
        return;
    }

    send_http(fd, 404, "application/json", R"({"error":"not_found"})");
}

void WebUI::serve_sse(int fd) {
    // Disable timeout for long-lived connection
    struct timeval tv{0, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    if (write(fd, header.data(), header.size()) <= 0) return;

    while (running_) {
        std::string data = trim_newlines(proxy("LIST"));
        std::string event = "data: " + data + "\n\n";
        ssize_t n = write(fd, event.data(), event.size());
        if (n <= 0) break;

        for (int i = 0; i < 10 && running_; i++) {
            usleep(100000);  // 100ms, total ~1s
        }
    }
}

std::string WebUI::proxy(const std::string &command) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return R"({"error":"connection_failed"})";

    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(control_port_));

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return R"({"error":"connection_failed"})";
    }

    std::string msg = command + "\n";
    if (write(fd, msg.data(), msg.size()) <= 0) {
        close(fd);
        return R"({"error":"write_failed"})";
    }

    std::string resp;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.find('\n') != std::string::npos) break;
    }

    close(fd);
    return trim_newlines(resp);
}

std::string WebUI::proxy_upload(const char *data, size_t len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return R"({"error":"connection_failed"})";

    struct timeval tv{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(control_port_));

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return R"({"error":"connection_failed"})";
    }

    std::string header = "UPLOAD " + std::to_string(len) + "\n";
    if (write(fd, header.data(), header.size()) <= 0) {
        close(fd);
        return R"({"error":"write_failed"})";
    }

    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) { close(fd); return R"({"error":"upload_write_failed"})"; }
        off += static_cast<size_t>(n);
    }

    std::string resp;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.find('\n') != std::string::npos) break;
    }

    close(fd);
    return trim_newlines(resp);
}

}  // namespace debuglantern
