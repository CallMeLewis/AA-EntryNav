// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Control protocol between the iDrive app and the Android Auto service: the real channel, driven by the app's own calls
// (control_address.hpp), and a real state file.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app_heartbeat.hpp"
#include "control_address.hpp"
#include "evo_control.hpp"
#include "service_contract.hpp"
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)
static std::vector<std::string> seen;
static void onCmd(const evo::ControlCommand& c, void*) {
    char b[48];
    snprintf(b, sizeof(b), "%s%s%d", c.verb.c_str(), c.hasArg ? ":" : "", c.hasArg ? c.arg : 0);
    seen.push_back(c.hasArg ? std::string(b) : c.verb);
}
static long nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static std::string text(const char* p) {
    std::ifstream f(p);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
int main() {
    evo::ControlCommand c;
    CHECK(evo::parseControlCommand("gear 2", 6, c) && c.verb == "gear" && c.hasArg && c.arg == 2);
    CHECK(evo::parseControlCommand("show\n", 5, c) && c.verb == "show" && !c.hasArg);
    CHECK(evo::parseControlCommand("await", 5, c) && c.verb == "await" && !c.hasArg);  // a phone waits for "show"
    CHECK(!evo::parseControlCommand("await 1", 7, c));
    CHECK(evo::parseControlCommand("drivemode 0 3 1", 15, c) == false);                 // 3 args: refused
    CHECK(evo::parseControlCommand("call 1", 6, c) && c.verb == "call" && c.arg == 1);  // phone keys during a call
    CHECK(evo::parseControlCommand("call", 4, c) == false);
    CHECK(evo::parseControlCommand("popup 1", 7, c) && c.verb == "popup" && c.arg == 1);  // iDrive popup
    CHECK(evo::parseControlCommand("popup", 5, c) == false);
    CHECK(evo::parseControlCommand("skip -1", 7, c) && c.verb == "skip" && c.arg == -1);  // iDrive's cluster menu
    CHECK(evo::parseControlCommand("clusterskip 1", 13, c) && c.verb == "clusterskip" && c.arg == 1);
    CHECK(evo::parseControlCommand("carsource 2", 11, c) && c.verb == "carsource" && c.arg == 2);  // iDrive's source
    CHECK(!evo::parseControlCommand("skip", 4, c) && !evo::parseControlCommand("clusterskip", 11, c));
    CHECK(!evo::parseControlCommand("carsource", 9, c));
    CHECK(evo::parseControlCommand("alive 7", 7, c) && c.verb == "alive" && c.arg == 7);  // the app's heartbeat
    CHECK(!evo::parseControlCommand("alive", 5, c));
    CHECK(!evo::parseControlCommand("gear", 4, c) && !evo::parseControlCommand("show 1", 6, c));
    CHECK(!evo::parseControlCommand("reboot", 6, c) && !evo::parseControlCommand("", 0, c));
    CHECK(!evo::parseControlCommand("gear 2 x", 8, c));

    {  // The app's heartbeat: nothing before the first "alive", one "silent" after 5 s, one "back", wrap-around safe.
        evo::AppHeartbeat beat;
        uint32_t quiet = 0;
        CHECK(beat.poll(60000, &quiet) == evo::AppHeartbeat::kNone);  // no app yet: never silent
        const uint32_t t0 = 0xfffff000u;                              // monoMs about to wrap
        beat.alive(t0);
        CHECK(beat.poll(t0 + 4999, &quiet) == evo::AppHeartbeat::kNone);
        CHECK(beat.poll(t0 + 5000, &quiet) == evo::AppHeartbeat::kSilent && quiet == 5000);
        CHECK(beat.poll(t0 + 9000, &quiet) == evo::AppHeartbeat::kNone);  // said once
        beat.alive(t0 + 12000);
        CHECK(beat.poll(t0 + 12100, &quiet) == evo::AppHeartbeat::kBack && quiet == 12000);
        CHECK(beat.poll(t0 + 13000, &quiet) == evo::AppHeartbeat::kNone);
        beat.alive(t0 + 13000);
        CHECK(beat.poll(t0 + 12999, &quiet) == evo::AppHeartbeat::kNone);  // an "alive" just after the poll's clock

        // HmiMain3D's threads, from a /proc laid out as the kernel does: a comm with spaces and parentheses, a thread
        // that runs (wchan 0) and one asleep in a futex.
        char root[] = "/tmp/aa-entrynav-proc-test-XXXXXX";
        CHECK(mkdtemp(root));
        const std::string r = root;
        const char* const dirs[] = {"/1", "/42", "/42/task", "/42/task/42", "/42/task/43", "/self"};
        for (const char* d : dirs) {
            CHECK(mkdir((r + d).c_str(), 0755) == 0);
        }
        struct File {
            const char* path;
            const char* text;
        } const files[] = {
            {"/1/comm", "init\n"},
            {"/42/comm", "HmiMain3D\n"},
            {"/42/task/42/comm", "HmiMain3D\n"},
            {"/42/task/42/stat", "42 (HmiMain3D) R 1 42\n"},
            {"/42/task/42/wchan", "0"},
            {"/42/task/43/comm", "bmw (disp) 2\n"},
            {"/42/task/43/stat", "43 (bmw (disp) 2) S 1 42\n"},
            {"/42/task/43/wchan", "futex_wait_queue_me"},
        };
        for (const File& f : files) {
            std::ofstream((r + f.path).c_str()) << f.text;
        }
        CHECK(evo::findProcess(root, "HmiMain3D") == 42 && evo::findProcess(root, "nothing") == 0);
        const std::string threads = evo::describeThreads(root, 42);
        CHECK(threads.find("42 HmiMain3D R 0\n") != std::string::npos);
        CHECK(threads.find("43 bmw (disp) 2 S futex_wait_queue_me\n") != std::string::npos);
        CHECK(evo::describeThreads(root, 7).empty() &&
              evo::describeThreads(root, 42, 1).find("...") != std::string::npos);
        // The real /proc: this test's own process and its thread.
        char self[16];
        snprintf(self, sizeof(self), "%d ", (int)getpid());
        CHECK(evo::describeThreads("/proc", (int)getpid()).find(self) != std::string::npos);
        // The app's step file: its first and third lines written, the second never (all zero).
        std::string steps(3 * 64, '\0');
        steps.replace(0, 64, std::string("status poll: hold (at 9 s)") + std::string(37, ' ') + "\n");
        steps.replace(128, 64, std::string("hmi job, cluster: done (at 8 s)") + std::string(32, ' ') + "\n");
        std::ofstream((r + "/steps").c_str()) << steps;
        CHECK(evo::readAppSteps((r + "/steps").c_str(), 64) ==
              "status poll: hold (at 9 s)\nhmi job, cluster: done (at 8 s)\n");
        CHECK(evo::readAppSteps((r + "/none").c_str(), 64).empty());
        const std::string rm = "rm -rf " + r;
        CHECK(system(rm.c_str()) == 0);
    }

    // The protocol version the app hands the supervisor: one spelling, and anything else (an older app gives none) is
    // refused.
    CHECK(std::string(evo::contract::kProtocolEnv) ==
          std::string(evo::contract::kProtocolVar) + "=" + evo::contract::kProtocol);
    CHECK(evo::contract::appProtocolMatches(evo::contract::kProtocol));
    CHECK(!evo::contract::appProtocolMatches(nullptr) && !evo::contract::appProtocolMatches("") &&
          !evo::contract::appProtocolMatches("1") && !evo::contract::appProtocolMatches("22"));

    // Who may send: root, or the service's own user.
    CHECK(evo::controlSenderAllowed(0, 1000) && evo::controlSenderAllowed(1000, 1000) &&
          evo::controlSenderAllowed(0, 0));
    CHECK(!evo::controlSenderAllowed(1001, 1000) && !evo::controlSenderAllowed(65534, 0));

    char name[48];
    snprintf(name, sizeof(name), "aa-entrynav-control-test-%d", (int)getpid());
    char state[] = "/tmp/aa-entrynav-state-test-XXXXXX";
    int sfd = mkstemp(state);
    CHECK(sfd >= 0);
    close(sfd);
    unlink(state);
    {  // The address: abstract (first byte 0), the name after it, nothing beyond; a name too long is cut.
        sockaddr_un a;
        const socklen_t length = evo::controlAddress("abc", a);
        CHECK(a.sun_family == AF_UNIX && a.sun_path[0] == 0 && !memcmp(a.sun_path + 1, "abc", 3));
        CHECK(length == offsetof(sockaddr_un, sun_path) + 4);
        const std::string huge(400, 'x');
        CHECK(evo::controlAddress(huge.c_str(), a) == sizeof(a));
    }
    {  // No service: the app is told at once, and never waits for an answer that cannot come.
        const long before = nowMs();
        CHECK(!evo::controlAnswers(name, 2000) && evo::sendControl(name, "show") == ECONNREFUSED);
        CHECK(nowMs() - before < 1000);
    }
    // The fault seen on a car: another program was given UDP port 52790 on 127.0.0.1, the old channel, and the
    // service gave up. Holding that port (when this machine lets us) no longer matters.
    const int stranger = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in old;
    memset(&old, 0, sizeof(old));
    old.sin_family = AF_INET;
    old.sin_port = htons(52790);
    old.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)bind(stranger, (sockaddr*)&old, sizeof(old));
    evo::EvoControl ctl;
    CHECK(ctl.start(name, state, onCmd, 0));
    close(stranger);
    evo::EvoControl second;
    CHECK(!second.start(name, state, onCmd, 0));  // one service only
    CHECK(evo::controlAnswers(name, 2000));       // ping -> pong, through the app's own call
    CHECK(!evo::sendControl(name, "show") && !evo::sendControl(name, "gear 2") && !evo::sendControl(name, "bogus") &&
          !evo::sendControl(name, "hide") && !evo::sendControl(name, "quit"));
    usleep(400000);
    CHECK(seen.size() == 4 && seen[0] == "show" && seen[1] == "gear:2" && seen[2] == "hide" && seen[3] == "quit");
    {  // A sender with no address of its own cannot be answered: the service must not stumble on its ping.
        const int mute = socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un to;
        const socklen_t length = evo::controlAddress(name, to);
        CHECK(sendto(mute, "ping", 4, 0, (sockaddr*)&to, length) == 4);
        close(mute);
        CHECK(evo::controlAnswers(name, 2000));
    }
    if (getuid() == 0) {  // Another user's program: neither its commands nor its pings are taken.
        seen.clear();
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            if (setgid(65534) != 0 || setuid(65534) != 0) {
                _exit(3);
            }
            const int sent = evo::sendControl(name, "quit");  // the kernel delivers it; the service drops it
            _exit(sent ? 4 : evo::controlAnswers(name, 300) ? 5 : 0);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        usleep(300000);
        CHECK(seen.empty());
        CHECK(!evo::sendControl(name, "show"));  // its own user still gets through
        usleep(300000);
        CHECK(seen.size() == 1 && seen[0] == "show");
    } else {
        puts("note: not root, the check of another user's commands is skipped");
    }
    ctl.writeState("connecting", 0);
    char expect[64];
    snprintf(expect, sizeof(expect), "connecting %d 0\n", (int)getpid());
    CHECK(text(state) == expect);
    ctl.writeState("connected", 1);
    snprintf(expect, sizeof(expect), "connected %d 1\n", (int)getpid());
    CHECK(text(state) == expect);
    ctl.stop();
    CHECK(access(state, F_OK) != 0);  // removed on stop: the app shows "Android Auto" again
    CHECK(!evo::controlAnswers(name, 2000) && evo::sendControl(name, "show") == ECONNREFUSED);  // the name is free
    {  // A holder of the name that never reads: not taken for a service, the wait is the one asked for, and once its
       // queue is full the app's sends fail at once instead of blocking iDrive's thread.
        const int silent = socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un a;
        const socklen_t length = evo::controlAddress(name, a);
        CHECK(bind(silent, (sockaddr*)&a, length) == 0);
        long before = nowMs();
        CHECK(!evo::controlAnswers(name, 150));
        CHECK(nowMs() - before >= 100 && nowMs() - before < 1500);
        evo::EvoControl refused;
        CHECK(!refused.start(name, state, onCmd, 0));
        before = nowMs();
        int full = 0;
        for (int i = 0; i < 5000 && !full; ++i) {
            full = evo::sendControl(name, "gear 2");
        }
        CHECK(full == EAGAIN);
        CHECK(nowMs() - before < 2000);
        close(silent);
    }
    evo::EvoControl again;
    CHECK(again.start(name, state, onCmd, 0) &&
          evo::controlAnswers(name, 2000));  // freed at once: a new service starts
    again.stop();
    puts(
        "PASS iDrive app control: abstract name with no port, ping/pong and commands through the app's own calls, "
        "unknown commands and other users refused, one service, no wait without a service or with a full queue, name "
        "freed on stop, protocol version, state file written atomically and removed on stop, the app's heartbeat, "
        "HmiMain3D's threads from /proc");
    return 0;
}
