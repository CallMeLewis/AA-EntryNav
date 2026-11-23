// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// pc-headunit: the project's Android Auto session against a real phone, on a PC. No car, nothing of BMW's.
//
// The phone runs Android Auto's "head unit server" (developer settings of the Android Auto app, then the menu's
// "Start head unit server") and is reached through `adb forward tcp:5277 tcp:5277`. This program connects there and
// runs core/aa_session exactly as the service does, with sound over Wi-Fi on. The phone's sound goes through
// platform/evo_audio to whatever libasound it is given (tests/fake_asound.c writes it to a file or a pipe).
//
//   pc-headunit <host> <port> <seconds> <video.h264>   the picture to a raw H.264 file; the media and navigation
//                                                      keys are pressed in turn, so it shows more than one screen
//   pc-headunit <host> <port> <seconds> web:<port>     a page served on that port shows the picture live in a
//                                                      browser (WebCodecs) and sends the mouse back as touches, the
//                                                      wheel as the knob, its buttons as keys. The phone is only
//                                                      called once the page is open. The panel's picture is the
//                                                      canvas's top rows shrunk to 1280x480 as the car does
//                                                      (PC_VIDEO=720 for the 1:1 picture); its Filter button
//                                                      switches bilinear and the browser's best, and Save picture
//                                                      downloads the exact pixels.
#include <arpa/inet.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "aa_input.hpp"
#include "aa_session.hpp"
#include "crash_handler.hpp"
#include "evo_audio.hpp"
#include "evo_log.hpp"
#include "evo_sdr_sensors.hpp"
#include "kombi_display.hpp"
#include "kombi_graph.hpp"
#include "kombi_guide.hpp"
#include "kombi_phone.hpp"
#include "sdr_builder.hpp"

namespace {

const int kKeyEveryS = 12;  // file mode: the next app's key this often

FILE* g_video = nullptr;  // file mode
int g_web = -1;           // web mode: the page's WebSocket
aa::AASession* g_session = nullptr;
unsigned g_frames = 0, g_audio[2] = {0, 0}, g_touches = 0;
volatile bool g_ended = false;
pthread_mutex_t g_webMu = PTHREAD_MUTEX_INITIALIZER;  // one message to the page at a time
pthread_mutex_t g_navMu = PTHREAD_MUTEX_INITIALIZER;
evo::kombi::NavInbox g_nav;  // the phone's newest navigation messages, under g_navMu

// The pretend drive: the car's GPS as the keyboard moves it. Off unless a start position is given; still until a
// drive key is pressed, so the phone gets the true position until then.
struct Drive {
    bool on;
    double lat, lon, heading, speed;  // degrees, degrees, degrees from north, metres per second
    int throttle, steer;              // -1, 0 or 1, from the page
};
Drive g_drive = {false, 0, 0, 0, 0, 0, 0};
pthread_mutex_t g_driveMu = PTHREAD_MUTEX_INITIALIZER;

// ---- the page ---------------------------------------------------------------------------------------------------

const char kPage[] = R"HTML(<!doctype html>
<meta charset="utf-8">
<title>Android Auto - aa-entrynav on the PC</title>
<style>
  html, body { margin: 0; height: 100%; background: #101214; color: #cfd3d8; font: 13px system-ui, sans-serif; }
  body { display: flex; flex-direction: column; align-items: center; }
  canvas { width: 100vw; max-width: 1280px; aspect-ratio: 1280 / 480; background: #000; touch-action: none; cursor: pointer; }
  #bar { display: flex; gap: 8px; padding: 8px; flex-wrap: wrap; justify-content: center; align-items: center; }
  button { background: #23272c; color: #e6e8eb; border: 1px solid #3a4047; border-radius: 6px; padding: 6px 14px; font: inherit; cursor: pointer; }
  button:hover { background: #2e343b; }
  #s { opacity: .75; margin-left: 10px; }
  #k { width: 100vw; max-width: 1280px; box-sizing: border-box; display: flex; align-items: center; gap: 18px;
       padding: 10px 16px; background: #05070a; border-top: 1px solid #2a2f36; min-height: 64px; }
  #k .t { font-size: 11px; letter-spacing: .08em; text-transform: uppercase; opacity: .6; width: 150px; }
  #ka { font-size: 40px; width: 56px; text-align: center; color: #fff; }
  #kd { font-size: 26px; color: #fff; min-width: 110px; }
  #kr { font-size: 20px; color: #fff; flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  #kc { font-size: 12px; opacity: .65; max-width: 420px; }
</style>
<canvas id="c" width="1280" height="480"></canvas>
<div id="k"><div class="t">Instrument cluster<br>would show</div><div id="ka"></div><div id="kd"></div>
  <div id="kr">no guidance</div><div id="kc"></div></div>
<div id="bar">
  <button data-k="3">Home</button><button data-k="4">Back</button>
  <button data-k="65537">Media</button><button data-k="65538">Nav</button><button data-k="65540">Phone</button>
  <button data-k="84">Assistant</button>
  <button data-r="-1">Knob ◀</button><button data-k="23">Knob press</button><button data-r="1">Knob ▶</button>
  <button id="f">Filter</button><button id="p">Save picture</button>
  <span id="s">connecting…</span>
</div>
<div id="bar"><span id="g">Drive: W faster, S brake, A / D steer, Space stop. Still until you press one.</span></div>
<script>
const c = document.getElementById('c'), ctx = c.getContext('2d'), s = document.getElementById('s');
let config = null, decoder = null, waitKey = true, shown = 0, smooth = 'low', mode = '';
// One canvas pixel on one screen pixel, as on the car, whatever the PC's display scaling.
const fit = () => { c.style.width = (1280 / devicePixelRatio) + 'px'; c.style.maxWidth = 'none'; };
fit(); matchMedia('(resolution: ' + devicePixelRatio + 'dppx)').addEventListener('change', fit);
// The 1080p picture is shrunk with bilinear filtering ('low', close to the car's compositor) or the browser's best.
document.getElementById('f').onclick = () => { smooth = smooth === 'high' ? 'low' : 'high'; };
// The panel's exact pixels as a PNG, named after the picture's size and filter.
document.getElementById('p').onclick = () => { const a = document.createElement('a');
  a.download = 'aa-' + mode.replace(/[^0-9a-z]+/gi, '-') + '.png'; a.href = c.toDataURL('image/png'); a.click(); };
const ws = new WebSocket('ws://' + location.host + '/ws');
ws.binaryType = 'arraybuffer';
const send = t => { if (ws.readyState === 1) ws.send(t); };

// The types of the NAL units of an Annex B buffer.
function nals(u) {
  const out = [];
  for (let i = 0; i + 3 < u.length; i++) {
    if (u[i] === 0 && u[i + 1] === 0 && u[i + 2] === 1) { out.push({ type: u[i + 3] & 31, at: i + 3 }); i += 3; }
  }
  return out;
}
function setup(u) {
  const sps = nals(u).find(n => n.type === 7);
  if (!sps) { s.textContent = 'no SPS in the configuration'; return; }
  const hex = b => b.toString(16).padStart(2, '0');
  const codec = 'avc1.' + hex(u[sps.at + 1]) + hex(u[sps.at + 2]) + hex(u[sps.at + 3]);
  if (decoder) decoder.close();
  decoder = new VideoDecoder({
    // The canvas's top rows, as wide as it is and 3/8 of its width high, over the whole panel.
    output: f => { const w = f.displayWidth, h = f.displayHeight;
      ctx.imageSmoothingEnabled = true; ctx.imageSmoothingQuality = smooth;
      ctx.drawImage(f, 0, 0, w, w * 3 / 8, 0, 0, 1280, 480); f.close();
      mode = w + 'x' + h + (w > 1280 ? ' shrunk, ' + smooth + ' filter' : ' 1:1');
      s.textContent = 'live, ' + mode + ', ' + (++shown) + ' pictures'; },
    error: e => { s.textContent = 'decoder: ' + e.message; waitKey = true; }
  });
  decoder.configure({ codec, optimizeForLatency: true });
  waitKey = true;
}
ws.onmessage = e => {
  const u = new Uint8Array(e.data), data = u.subarray(1);
  if (u[0] === 2) {
    const f = new TextDecoder().decode(data).split('\n');
    document.getElementById('ka').textContent = f[0];
    document.getElementById('kd').textContent = f[1];
    document.getElementById('kr').textContent = f[2];
    document.getElementById('kc').textContent = f[3];
    return;
  }
  if (u[0] === 3) { document.getElementById('g').textContent = new TextDecoder().decode(data); return; }
  if (u[0] === 0) { config = data.slice(); setup(config); return; }
  if (!decoder || decoder.state !== 'configured') return;
  const key = nals(data.subarray(0, 64)).some(n => n.type === 5);
  if (waitKey && !key) return;
  waitKey = false;
  let payload = data;
  if (key && config) { payload = new Uint8Array(config.length + data.length); payload.set(config); payload.set(data, config.length); }
  decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: performance.now() * 1000, data: payload }));
};
ws.onopen = () => { s.textContent = 'calling the phone…'; };
ws.onclose = () => { s.textContent = 'session ended'; };
if (!('VideoDecoder' in window)) s.textContent = 'this browser has no WebCodecs: use Edge or Chrome';

// The mouse as one finger, in the phone's coordinates (the car's screen is the top 480 rows of 1280 x 720).
let down = false;
const at = ev => { const r = c.getBoundingClientRect(); return Math.round((ev.clientX - r.left) * 1280 / r.width) + ' ' + Math.round((ev.clientY - r.top) * 480 / r.height); };
c.onpointerdown = ev => { down = true; c.setPointerCapture(ev.pointerId); send('t 0 ' + at(ev)); };
c.onpointermove = ev => { if (down) send('t 2 ' + at(ev)); };
c.onpointerup = c.onpointercancel = ev => { if (down) { down = false; send('t 1 ' + at(ev)); } };
c.addEventListener('wheel', ev => { ev.preventDefault(); send('r ' + (ev.deltaY > 0 ? 1 : -1)); }, { passive: false });
const key = k => { send('k ' + k + ' 1'); send('k ' + k + ' 0'); };
document.querySelectorAll('button:not(#f):not(#p)').forEach(b => b.onclick = () => b.dataset.k ? key(b.dataset.k) : send('r ' + b.dataset.r));
const keys = { ArrowUp: 19, ArrowDown: 20, ArrowLeft: 21, ArrowRight: 22, Enter: 23, Escape: 4, Backspace: 4, h: 3 };
const held = new Set();
const drive = () => send('d ' + ((held.has('w') ? 1 : 0) - (held.has('s') ? 1 : 0)) + ' ' + ((held.has('d') ? 1 : 0) - (held.has('a') ? 1 : 0)));
document.onkeydown = ev => {
  const k = ev.key.toLowerCase();
  if ('wasd'.includes(k) && k.length === 1) { ev.preventDefault(); if (!held.has(k)) { held.add(k); drive(); } return; }
  if (ev.key === ' ') { ev.preventDefault(); send('x'); return; }
  if (keys[ev.key]) { ev.preventDefault(); key(keys[ev.key]); }
};
document.onkeyup = ev => { const k = ev.key.toLowerCase(); if (held.delete(k)) drive(); };
window.onblur = () => { held.clear(); drive(); };
</script>
)HTML";

// ---- sockets ----------------------------------------------------------------------------------------------------

bool sendAll(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len) {
        const ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool readAll(int fd, uint8_t* to, size_t len) {
    while (len) {
        const ssize_t n = recv(fd, to, len, 0);
        if (n <= 0) {
            return false;
        }
        to += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

int connectTo(const char* host, const char* port) {
    struct addrinfo hints, *found = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &found) != 0 || !found) {
        return -1;
    }
    const int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd >= 0 && connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(found);
        return -1;
    }
    freeaddrinfo(found);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// ---- the page's WebSocket ---------------------------------------------------------------------------------------

// One binary message to the page: `kind` (0 configuration, 1 picture, 2 the cluster strip's text), then the data.
bool webSend(uint8_t kind, const uint8_t* data, size_t len) {
    const uint64_t size = len + 1;
    uint8_t header[10];
    size_t n = 2;
    header[0] = 0x82;  // final, binary
    if (size < 126) {
        header[1] = static_cast<uint8_t>(size);
    } else if (size < 65536) {
        header[1] = 126;
        header[2] = static_cast<uint8_t>(size >> 8);
        header[3] = static_cast<uint8_t>(size);
        n = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; ++i) {
            header[2 + i] = static_cast<uint8_t>(size >> (56 - 8 * i));
        }
        n = 10;
    }
    pthread_mutex_lock(&g_webMu);
    const bool ok = sendAll(g_web, header, n) && sendAll(g_web, &kind, 1) && sendAll(g_web, data, len);
    pthread_mutex_unlock(&g_webMu);
    return ok;
}

// A command of the page: "t <action> <x> <y>" a touch, "k <code> <down>" a key, "r <steps>" the knob.
void webCommand(const std::string& text) {
    int a = 0, b = 0, c = 0;
    if (!g_session) {
        return;
    }
    if (sscanf(text.c_str(), "t %d %d %d", &a, &b, &c) == 3) {
        aa::TouchEventOut touch;
        memset(&touch, 0, sizeof(touch));
        touch.n = 1;
        touch.p[0].id = 0;
        touch.p[0].x = b < 0 ? 0 : b > 1279 ? 1279 : b;
        touch.p[0].y = c < 0 ? 0 : c > 479 ? 479 : c;
        touch.action = a;
        touch.index = 0;
        const aa::Canvas& canvas = g_session->canvas();  // the page's pixels on the phone's canvas
        touch.p[0].x = touch.p[0].x * static_cast<int>(canvas.width) / 1280;
        touch.p[0].y = touch.p[0].y * static_cast<int>(canvas.shownHeight) / 480;
        g_session->postTouchEvent(touch);
        if (a != 2 && ++g_touches <= 5) {
            ELOG("PC", "touch %s at %d,%d", a == 0 ? "down" : "up", touch.p[0].x, touch.p[0].y);
        }
    } else if (sscanf(text.c_str(), "k %d %d", &a, &b) == 2) {
        g_session->postKeyEvent(static_cast<uint32_t>(a), b != 0);
    } else if (sscanf(text.c_str(), "r %d", &a) == 1) {
        g_session->postRelativeEvent(aa::AAK_ROTARY_CONTROLLER, a);
    } else if (sscanf(text.c_str(), "d %d %d", &a, &b) == 2) {
        pthread_mutex_lock(&g_driveMu);
        g_drive.throttle = a > 0 ? 1 : a < 0 ? -1 : 0;
        g_drive.steer = b > 0 ? 1 : b < 0 ? -1 : 0;
        pthread_mutex_unlock(&g_driveMu);
    } else if (text == "x") {
        pthread_mutex_lock(&g_driveMu);
        g_drive.speed = 0;
        g_drive.throttle = 0;
        pthread_mutex_unlock(&g_driveMu);
    }
}

// Reads the page's messages until it closes.
void* webReader(void*) {
    for (;;) {
        uint8_t head[2], mask[4] = {0, 0, 0, 0};
        if (!readAll(g_web, head, 2)) {
            break;
        }
        uint64_t size = head[1] & 0x7f;
        if (size == 126) {
            uint8_t more[2];
            if (!readAll(g_web, more, 2)) {
                break;
            }
            size = (more[0] << 8) | more[1];
        } else if (size == 127) {
            break;  // the page never sends that much
        }
        if ((head[1] & 0x80) && !readAll(g_web, mask, 4)) {
            break;
        }
        std::vector<uint8_t> body(size);
        if (size && !readAll(g_web, &body[0], size)) {
            break;
        }
        for (size_t i = 0; i < body.size(); ++i) {
            body[i] ^= mask[i & 3];
        }
        const int opcode = head[0] & 0x0f;
        if (opcode == 8) {
            break;
        }
        if (opcode == 1) {
            webCommand(std::string(body.begin(), body.end()));
        }
    }
    g_ended = true;
    return nullptr;
}

// Serves the page on the port, and returns the socket of its WebSocket once it has connected; -1 after two minutes.
int waitForPage(int port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (listener < 0 || bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 4) != 0) {
        return -1;
    }
    ELOG("PC", "the page is at http://127.0.0.1:%d/ : waiting for it to open", port);
    struct timeval limit = {120, 0};
    setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    for (;;) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            close(listener);
            return -1;
        }
        struct timeval patience = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
        std::string request;
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            request.append(buf, static_cast<size_t>(n));
        }
        const size_t keyAt = request.find("Sec-WebSocket-Key: ");
        if (keyAt != std::string::npos) {
            const size_t from = keyAt + 19, to = request.find("\r\n", from);
            const std::string key = request.substr(from, to - from) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            unsigned char digest[SHA_DIGEST_LENGTH], encoded[64];
            SHA1(reinterpret_cast<const unsigned char*>(key.data()), key.size(), digest);
            EVP_EncodeBlock(encoded, digest, SHA_DIGEST_LENGTH);
            const std::string answer = std::string(
                                           "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                           "Connection: Upgrade\r\nSec-WebSocket-Accept: ") +
                                       reinterpret_cast<const char*>(encoded) + "\r\n\r\n";
            sendAll(fd, answer.data(), answer.size());
            struct timeval forever = {0, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &forever, sizeof(forever));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            close(listener);
            ELOG("PC", "the page is open");
            return fd;
        }
        const bool page = request.compare(0, 6, "GET / ") == 0;
        char head[160];
        snprintf(
            head, sizeof(head),
            "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            page ? "200 OK" : "404 Not Found", page ? static_cast<unsigned>(sizeof(kPage) - 1) : 0u);
        sendAll(fd, head, strlen(head));
        if (page) {
            sendAll(fd, kPage, sizeof(kPage) - 1);
        }
        close(fd);
    }
}

// ---- the session's callbacks ------------------------------------------------------------------------------------

// To the file or the page, at once; a page that left ends the run.
void put(uint8_t kind, const uint8_t* data, size_t len) {
    const bool ok =
        g_web >= 0 ? webSend(kind, data, len) : fwrite(data, 1, len, g_video) == len && fflush(g_video) == 0;
    if (!ok) {
        g_ended = true;
    }
}

void onCodec(const uint8_t* data, size_t len, void*) {
    put(0, data, len);
    ELOG("PC", "H.264 configuration: %u bytes", static_cast<unsigned>(len));
}

void onVideo(uint32_t, const uint8_t* data, size_t len, uint64_t, void*) {
    put(1, data, len);
    if (++g_frames == 1) {
        ELOG("PC", "first picture from the phone");
    }
}

void onAudio(uint8_t channel, const uint8_t* pcm, size_t len, uint64_t pts, void* user) {
    ++g_audio[channel == 9 ? 1 : 0];
    evo::EvoAudio::onData(channel, pcm, len, pts, user);
}

void onEnded(void*) {
    g_ended = true;
}

void onNav(uint16_t id, const uint8_t* body, size_t size, void*) {
    pthread_mutex_lock(&g_navMu);
    g_nav.keep(id, body, size);
    pthread_mutex_unlock(&g_navMu);
}

// ---- the pretend drive ------------------------------------------------------------------------------------------

// The description with the car's LOCATION sensor kept, as on a car whose GPS the service reads.
int keepLocationTrim(std::vector<uint8_t>& sdr) {
    return evo::filterSdrSensors(sdr, evo::keptSensorTypes(true));
}

// A tenth of a second of driving, and the car's position to the phone five times a second.
void driveTick(aa::AASession& session, int tenth) {
    const double dt = 0.1, kMaxSpeed = 31.3;  // 70 mph
    pthread_mutex_lock(&g_driveMu);
    Drive& d = g_drive;
    d.speed += d.throttle > 0 ? 2.5 * dt : d.throttle < 0 ? -7.0 * dt : -0.25 * dt;
    d.speed = d.speed < 0 ? 0 : d.speed > kMaxSpeed ? kMaxSpeed : d.speed;
    d.heading = fmod(d.heading + d.steer * 40.0 * dt + 360.0, 360.0);
    const double radians = d.heading * M_PI / 180.0;
    d.lat += d.speed * dt * cos(radians) / 111320.0;
    d.lon += d.speed * dt * sin(radians) / (111320.0 * cos(d.lat * M_PI / 180.0));
    const Drive now = d;
    pthread_mutex_unlock(&g_driveMu);
    if (tenth % 2 == 0) {
        aa::CarLocation fix;
        fix.latitudeE6 = static_cast<int32_t>(lround(now.lat * 1e6));
        fix.longitudeE6 = static_cast<int32_t>(lround(now.lon * 1e6));
        fix.sampledMs = aa::locationClockMs();
        fix.valid = true;
        fix.hasSpeed = fix.hasBearing = fix.hasAccuracy = true;
        fix.speedMps = now.speed;
        fix.bearingDeg = now.heading;
        fix.accuracyM = 4;
        fix.utcSeconds = static_cast<uint64_t>(time(nullptr));
        session.postCarLocation(fix);
    }
    if (tenth % 5 == 0 && g_web >= 0) {
        char text[160];
        snprintf(text, sizeof(text),
                 "Drive: W faster, S brake, A / D steer, Space stop.   %.0f mph, heading %.0f\xc2\xb0%s",
                 now.speed * 2.237, now.heading, now.speed > 0 ? "" : "   (still: the phone has your true position)");
        webSend(3, reinterpret_cast<const uint8_t*>(text), strlen(text));
    }
}

// ---- what the instrument cluster would show -----------------------------------------------------------------------

// The UTF-16 text after a model's byte-order mark, as UTF-8 (the cluster's strings are UTF-16, big-endian).
std::string textOf(const std::vector<uint8_t>& b, int* after = nullptr) {
    for (size_t mark = 18; mark + 1 < b.size(); ++mark) {
        if (b[mark] == 0xFE && b[mark + 1] == 0xFF) {
            const size_t end = mark + b[mark - 1];
            std::string out;
            for (size_t k = mark + 2; k + 2 < end && k + 1 < b.size(); k += 2) {
                const unsigned c = (b[k] << 8) | b[k + 1];
                if (c < 0x80) {
                    out += static_cast<char>(c);
                } else if (c < 0x800) {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            if (after) {
                *after = end < b.size() ? b[end] : -1;
            }
            return out;
        }
    }
    return std::string();
}

// The guide's requests read back as the cluster would: its road line and its distance, from the very bytes the car
// would be sent (the models of tests/test_kombi_guide.cpp: 0x0003 type 3 the road, 0x000A type 29 the distance).
class ClusterPanel : public evo::kombi::GuideOutput {
public:
    ClusterPanel() : unit(-1), requests(0), cancels(0) {}
    evo::kombi::SendOutcome send(const std::vector<evo::kombi::Request>& batch, std::string*) override {
        for (size_t i = 0; i < batch.size(); ++i) {
            const std::vector<uint8_t>& b = batch[i].body;
            const int type = b.size() > 8 ? b[8] : 0;
            const unsigned index = b.size() >= 8 ? (b[5] << 16 | b[6] << 8 | b[7]) : 1;
            if (batch[i].method == 0x0003 && type == 3 && index == 0) {
                road = textOf(b);
            } else if (batch[i].method == 0x000A && type == 29 && index == 0) {
                distance = textOf(b, &unit);
            }
        }
        requests += batch.size();
        return evo::kombi::kSent;
    }
    bool cancel(std::string*) override {
        ++cancels;
        road.clear();
        distance.clear();
        return true;
    }
    std::string road, distance;
    int unit;
    size_t requests;
    unsigned cancels;
};

const char* arrowOf(int direction) {
    using namespace evo::kombi;
    switch (direction) {
        case kStraightOn:
            return "\xe2\x86\x91";
        case kHalfLeft:
        case kForkLeft:
        case kHighwayOutLeft:
        case kHighwayInLeft:
        case kLaneChangeLeft:
            return "\xe2\x86\x96";
        case kHalfRight:
        case kForkRight:
        case kHighwayOutRight:
        case kHighwayInRight:
        case kLaneChangeRight:
            return "\xe2\x86\x97";
        case kLeft:
        case kTightLeft:
            return "\xe2\x86\x90";
        case kRight:
        case kTightRight:
            return "\xe2\x86\x92";
        case kUTurnLeft:
        case kUTurnRight:
            return "\xe2\x86\xa9";
        case kDestination:
            return "\xe2\x9a\x91";
        case kRoundaboutClockwise:
            return "\xe2\x86\xbb";
        case kRoundaboutCounterClockwise:
            return "\xe2\x86\xba";
        default:
            return "?";
    }
}

// One tick of the car's guide (platform/kombi_guide.cpp, with the car's own settings) on the phone's newest
// messages; the strip's text goes to the page when it changed.
void clusterTick(evo::kombi::Guide& guide, ClusterPanel& panel, std::string& last) {
    pthread_mutex_lock(&g_navMu);
    const evo::kombi::NavInbox nav = g_nav;
    pthread_mutex_unlock(&g_navMu);
    evo::kombi::Maneuver m;
    const bool has = nav.guiding() && nav.nextManeuver(m);
    guide.step(has ? &m : nullptr, evo::upSec());
    std::string text;
    if (guide.showing()) {
        text = std::string(arrowOf(evo::kombi::clusterDirection(m.type))) + "\n" + panel.distance + " " +
               evo::kombi::clusterUnitName(panel.unit) + "\n" + panel.road + "\nphone said: \"" + m.cue + "\"";
    } else if (has) {
        double metres = 0;
        char far[96];
        if (evo::kombi::metresToManeuver(m, metres)) {
            snprintf(far, sizeof(far), "arrow hidden: next turn %.1f mi away, shown from 1 mi", metres / 1609.344);
        } else {
            snprintf(far, sizeof(far), "arrow not shown yet");
        }
        text = std::string("\n\n") + far + "\nphone said: \"" + m.cue + "\"";
    } else {
        text = "\n\nno guidance\n";
    }
    if (text != last && g_web >= 0) {
        last = text;
        webSend(2, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        fputs("usage: pc-headunit <host> <port> <seconds> <video.h264 | web:port>\n", stderr);
        return 2;
    }
    const int seconds = atoi(argv[3]);
    aa::installProcessGuards();
    const bool web = strncmp(argv[4], "web:", 4) == 0;
    if (web) {
        g_web = waitForPage(atoi(argv[4] + 4));
    } else {
        g_video = fopen(argv[4], "wb");
    }
    if (seconds < 1 || (web ? g_web < 0 : !g_video)) {
        fputs("pc-headunit: bad duration, or no video file, or the page never opened\n", stderr);
        return 2;
    }
    const int fd = connectTo(argv[1], argv[2]);
    if (fd < 0) {
        ELOG("PC", "cannot connect to %s:%s: is the phone's head unit server started, and adb forward set?", argv[1],
             argv[2]);
        return 1;
    }
    ELOG("PC", "connected to the phone at %s:%s", argv[1], argv[2]);
    evo::EvoAudio audio;
    evo::AudioSettings settings;
    if (!audio.start(settings)) {
        return 1;
    }
    {
        aa::AASession session;
        session.setDisconnectCallback(onEnded);
        session.setCodecConfigCallback(onCodec);
        session.setVideoFrameCallback(onVideo);
        session.setAudioCallbacks(onAudio, evo::EvoAudio::onStream, &audio);
        session.setNavStatusCallback(onNav);
        // PC_VIDEO=720: the 1280x720 picture shown 1:1, as with the car's video-720 file; else 1920x1080 shrunk.
        session.setCanvas(aa::canvasFor(!(getenv("PC_VIDEO") && !strcmp(getenv("PC_VIDEO"), "720"))));
        // A vehicle file (model=, year=, id=) as the service reads on the car, when one is given.
        if (getenv("PC_VEHICLE_FILE")) {
            session.setVehicleFile(getenv("PC_VEHICLE_FILE"));
        }
        // "lat,lon": the pretend drive starts there, and the phone is told the car has its own GPS.
        if (web && getenv("PC_DRIVE_START") &&
            sscanf(getenv("PC_DRIVE_START"), "%lf,%lf", &g_drive.lat, &g_drive.lon) == 2) {
            g_drive.on = true;
            aa::setDiscoveryTrim(keepLocationTrim);
            ELOG("PC", "pretend drive ready: the phone gets the car's position from this program");
        }
        if (!session.start(fd)) {
            return 1;
        }
        g_session = &session;
        pthread_t reader;
        const bool reading = web && pthread_create(&reader, nullptr, webReader, nullptr) == 0;
        const int keys[] = {aa::AAK_MEDIA, aa::AAK_NAVIGATION};
        const evo::kombi::RouteGraph graph;
        ClusterPanel panel;
        evo::kombi::Guide guide(graph, panel, evo::kombi::GuideSettings());
        std::string strip;
        for (int tenth = 1; tenth <= seconds * 10 && !g_ended; ++tenth) {
            usleep(100000);
            if (tenth % 3 == 0) {
                clusterTick(guide, panel, strip);
            }
            if (g_drive.on) {
                driveTick(session, tenth);
            }
            if (!web && tenth % (kKeyEveryS * 10) == 0) {
                const int key = keys[(tenth / (kKeyEveryS * 10) - 1) % 2];
                session.postKeyEvent(static_cast<uint32_t>(key), true);
                session.postKeyEvent(static_cast<uint32_t>(key), false);
                ELOG("PC", "key %s", key == aa::AAK_MEDIA ? "MEDIA" : "NAVIGATION");
            }
        }
        ELOG("PC", "%s", g_ended ? "the phone or the page ended the session" : "time is up: stopping");
        session.stop();
        g_session = nullptr;
        if (web) {
            shutdown(g_web, SHUT_RDWR);
            if (reading) {
                pthread_join(reader, nullptr);
            }
            close(g_web);
        }
    }
    audio.sessionEnded();
    audio.stop();
    if (g_video) {
        fclose(g_video);
    }
    ELOG("PC", "summary: %u picture(s), %u speech message(s), %u media message(s), %u touch(es)", g_frames, g_audio[0],
         g_audio[1], g_touches);
    return g_frames ? 0 : 3;
}
