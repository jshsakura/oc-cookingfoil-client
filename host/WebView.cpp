#include "host/WebView.hpp"
#include <SDL2/SDL_image.h>
#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
namespace foil::host {
namespace {
constexpr int FrameMs = 33;
constexpr size_t MaxPng = 16 * 1024 * 1024;
const char *Page = R"HTML(<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CookingFoil 미리보기</title>
<style>
:root{--bg:#F2F1EE;--ink:#16161A;--line:#D4D2CB;--butter:#FFC23D}
body{margin:0;background:var(--bg);color:var(--ink);font:15px system-ui,sans-serif}
main{max-width:1280px;margin:0 auto;padding:16px}
img{width:100%;aspect-ratio:16/9;display:block;background:#111;cursor:pointer;touch-action:none}
.pad{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
button{font:inherit;font-weight:700;border:0;border-radius:999px;padding:10px 16px;background:var(--ink);color:#fff;cursor:pointer}
button.alt{background:var(--butter);color:var(--ink)}
p{color:#5b5b63;margin:10px 0 0}
</style></head><body><main>
<img id="screen" alt="앱 화면">
<div class="pad">
<button data-k="UP">↑</button><button data-k="DOWN">↓</button><button data-k="LEFT">←</button><button data-k="RIGHT">→</button>
<button class="alt" data-k="A">A</button><button data-k="B">B</button><button data-k="X">X</button><button data-k="Y">Y</button>
<button data-k="L">L</button><button data-k="R">R</button><button data-k="ZR">ZR</button>
<button data-k="SUP">R스틱 ↑</button><button data-k="SDOWN">R스틱 ↓</button><button data-k="PLUS">+</button>
</div>
<p>화면을 누르면 터치, 끌면 스크롤입니다. 키보드는 방향키, Enter=A, Esc=B, X, Y, L, R, Z=ZR, PageUp/PageDown=R스틱입니다.</p>
</main><script>
const img=document.getElementById('screen');
const send=(path)=>fetch(path).catch(()=>{});
const key=(k)=>send('/key?k='+k);
document.querySelectorAll('[data-k]').forEach(b=>b.onclick=()=>key(b.dataset.k));
const keys={ArrowUp:'UP',ArrowDown:'DOWN',ArrowLeft:'LEFT',ArrowRight:'RIGHT',Enter:'A',Escape:'B',Backspace:'B',
  x:'X',y:'Y',l:'L',r:'R',z:'ZR',PageUp:'SUP',PageDown:'SDOWN','+':'PLUS'};
addEventListener('keydown',e=>{const k=keys[e.key];if(k){e.preventDefault();key(k);}});
let start=null,last=0,moved=false;
const at=(e)=>{const r=img.getBoundingClientRect();return[(e.clientX-r.left)*1280/r.width,(e.clientY-r.top)*720/r.height];};
img.onpointerdown=e=>{start=at(e);last=start[1];moved=false;img.setPointerCapture(e.pointerId);};
img.onpointermove=e=>{if(!start)return;const p=at(e);if(Math.abs(p[1]-start[1])>14)moved=true;
  if(moved){send('/drag?dy='+(p[1]-last).toFixed(1));last=p[1];}};
img.onpointerup=e=>{if(start&&!moved){const p=at(e);send('/tap?x='+p[0].toFixed(1)+'&y='+p[1].toFixed(1));}start=null;};
async function loop(){
  try{const r=await fetch('/frame.png?'+Date.now());
    if(r.headers.get('X-Search')){const q=prompt('게임 이름으로 찾기','');if(q!==null)send('/search?q='+encodeURIComponent(q));}
    const u=URL.createObjectURL(await r.blob());
    const old=img.src;img.src=u;if(old)URL.revokeObjectURL(old);}catch(e){}
  setTimeout(loop,120);
}
loop();
</script></body></html>)HTML";
std::string decode(const std::string &text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      out += char(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else
      out += text[i] == '+' ? ' ' : text[i];
  }
  return out;
}
std::map<std::string, std::string> query(const std::string &target) {
  std::map<std::string, std::string> result;
  const auto mark = target.find('?');
  if (mark == std::string::npos)
    return result;
  size_t at = mark + 1;
  while (at < target.size()) {
    const auto end = std::min(target.find('&', at), target.size());
    const auto pair = target.substr(at, end - at);
    const auto equals = pair.find('=');
    if (equals != std::string::npos)
      result[pair.substr(0, equals)] = decode(pair.substr(equals + 1));
    at = end + 1;
  }
  return result;
}
void reply(int client, const std::string &type, const std::string &body,
           const std::string &extra = "") {
  const auto head = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\n" + extra +
                    "Cache-Control: no-store\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  for (const auto *part : {&head, &body}) {
    size_t sent = 0;
    while (sent < part->size()) {
      const auto n = send(client, part->data() + sent, part->size() - sent, MSG_NOSIGNAL);
      if (n <= 0)
        return;
      sent += size_t(n);
    }
  }
}
std::string frame(SDL_Renderer *renderer) {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  auto *shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
  if (!shot)
    return {};
  std::string png;
  if (SDL_RenderReadPixels(renderer, nullptr, shot->format->format, shot->pixels, shot->pitch) == 0) {
    std::vector<char> buffer(MaxPng);
    auto *rw = SDL_RWFromMem(buffer.data(), int(buffer.size()));
    if (rw && IMG_SavePNG_RW(shot, rw, 0) == 0)
      png.assign(buffer.data(), size_t(SDL_RWtell(rw)));
    if (rw)
      SDL_RWclose(rw);
  }
  SDL_FreeSurface(shot);
  return png;
}
void input(ui::Application &app, const std::string &k) {
  using ui::Action;
  static const std::map<std::string, Action> actions = {
      {"UP", Action::Up},       {"DOWN", Action::Down},     {"LEFT", Action::Left},
      {"RIGHT", Action::Right}, {"A", Action::Accept},      {"B", Action::Back},
      {"L", Action::Previous},  {"R", Action::Next},        {"ZR", Action::Preview},
      {"SUP", Action::ScrollUp}, {"SDOWN", Action::ScrollDown}};
  if (k == "Y")
    app.action(app.current() == ui::Screen::Settings || app.current() == ui::Screen::Installed
                   ? Action::Reload
                   : Action::Queue);
  else if (k == "X" && app.current() == ui::Screen::Library)
    app.requestSearch();
  else if (k == "X")
    app.action(Action::Search);
  else if (k == "PLUS")
    app.requestQuit();
  else if (k == "MINUS")
    app.refreshAll();
  else if (const auto found = actions.find(k); found != actions.end())
    app.action(found->second);
}
} // namespace
int serveWeb(ui::Application &app, ui::Runtime &runtime, int port) {
  std::signal(SIGPIPE, SIG_IGN);
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  const int yes = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(uint16_t(port));
  if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      listen(server, 16) != 0) {
    std::perror("serve");
    return 1;
  }
  std::printf("Web preview: http://0.0.0.0:%d/\n", port);
  std::fflush(stdout);
  for (;;) {
    // SDL turns SIGINT/SIGTERM into SDL_QUIT, so stop on it.
    SDL_Event event;
    while (SDL_PollEvent(&event))
      if (event.type == SDL_QUIT) {
        close(server);
        return 0;
      }
    app.render();
    pollfd waiting{server, POLLIN, 0};
    if (poll(&waiting, 1, FrameMs) <= 0)
      continue;
    const int client = accept(server, nullptr, nullptr);
    if (client < 0)
      continue;
    char buffer[8192]{};
    const auto n = recv(client, buffer, sizeof(buffer) - 1, 0);
    std::string request(buffer, n > 0 ? size_t(n) : 0);
    const auto first = request.find(' '), second = request.find(' ', first + 1);
    const auto target = first == std::string::npos ? "/" : request.substr(first + 1, second - first - 1);
    const auto path = target.substr(0, target.find('?'));
    auto args = query(target);
    if (path == "/frame.png") {
      // A search request (search bar tap, X) asks the page for a query.
      const std::string search = app.takeSearchRequest() ? "X-Search: 1\r\nAccess-Control-Expose-Headers: X-Search\r\n" : "";
      reply(client, "image/png", frame(runtime.renderer()), search);
    } else if (path == "/key") {
      input(app, args["k"]);
      reply(client, "text/plain", "ok");
    } else if (path == "/tap") {
      app.touch(std::strtof(args["x"].c_str(), nullptr), std::strtof(args["y"].c_str(), nullptr));
      reply(client, "text/plain", "ok");
    } else if (path == "/drag") {
      app.drag(std::strtof(args["dy"].c_str(), nullptr));
      reply(client, "text/plain", "ok");
    } else if (path == "/search") {
      app.search(args["q"]);
      reply(client, "text/plain", "ok");
    } else {
      reply(client, "text/html; charset=utf-8", Page);
    }
    close(client);
  }
}
} // namespace foil::host
