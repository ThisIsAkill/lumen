// lumen-expand-helper: lets Lumen expand snippet keywords typed in any app.
//
// Wayland does not let applications see what is typed in other windows, so
// this small system service (run by systemd as root, see
// lumen-expand.service) reads the keyboards itself. It keeps only the last
// few typed characters in memory, never writes them anywhere, and tells
// Lumen nothing but "the keyword ;addr was just typed", over a socket only
// the configured user can open. Lumen then replaces the keyword.
//
// Protocol (lines of UTF-8 text on the socket):
//   Lumen  → helper: "keywords\t;addr\t;sig\n"   the keywords to watch for
//   helper → Lumen:  "match\t;addr\n"            one was just typed
//
// Options: --config FILE (default /etc/lumen-expand.conf: user=, layout=,
// variant=), --socket PATH (default /run/lumen-expand/socket), and
// --test-events, which reads key events from standard input instead of the
// keyboards (for the tests; no devices are opened).

#include "keymatcher.h"

#include <libevdev/libevdev.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct Config {
    std::string user;
    std::string layout = "us";
    std::string variant;
};

Config readConfig(const std::string &path)
{
    Config config;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "user")
            config.user = value;
        else if (key == "layout")
            config.layout = value;
        else if (key == "variant")
            config.variant = value;
    }
    return config;
}

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator))
        parts.push_back(part);
    return parts;
}

// A keyboard (or a pointer, whose clicks only reset what was typed).
struct Device {
    libevdev *dev = nullptr;
    int fd = -1;
    bool keyboard = false;
};

class Helper
{
public:
    Helper(const Config &config, std::string socketPath, bool testEvents)
        : m_matcher(config.layout, config.variant)
        , m_socketPath(std::move(socketPath))
        , m_testEvents(testEvents)
        , m_user(config.user)
    {
    }

    ~Helper()
    {
        for (auto &[path, d] : m_devices)
            closeDevice(d);
        if (m_client >= 0)
            close(m_client);
        if (m_listen >= 0) {
            close(m_listen);
            unlink(m_socketPath.c_str());
        }
    }

    bool start()
    {
        if (!m_matcher.valid()) {
            std::fprintf(stderr, "lumen-expand-helper: unknown keyboard layout\n");
            return false;
        }
        if (!listenOnSocket())
            return false;
        if (!m_testEvents) {
            m_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            inotify_add_watch(m_inotify, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE);
            scanDevices();
        }
        return true;
    }

    void run(const volatile std::sig_atomic_t &stop)
    {
        while (!stop) {
            std::vector<pollfd> fds;
            fds.push_back({m_listen, POLLIN, 0});
            if (m_client >= 0)
                fds.push_back({m_client, POLLIN, 0});
            if (m_inotify >= 0)
                fds.push_back({m_inotify, POLLIN, 0});
            if (m_testEvents)
                fds.push_back({STDIN_FILENO, POLLIN, 0});
            for (const auto &[path, d] : m_devices)
                fds.push_back({d.fd, POLLIN, 0});
            if (poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR)
                    continue;
                return;
            }
            for (const pollfd &p : fds) {
                if (!(p.revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                if (p.fd == m_listen)
                    acceptClient();
                else if (p.fd == m_client)
                    readClient();
                else if (p.fd == m_inotify)
                    deviceFolderChanged();
                else if (m_testEvents && p.fd == STDIN_FILENO) {
                    if (!readTestEvents())
                        return; // input ended: the test is over
                } else
                    readDevice(p.fd);
            }
        }
    }

private:
    bool listenOnSocket()
    {
        m_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (m_socketPath.size() >= sizeof addr.sun_path)
            return false;
        std::strcpy(addr.sun_path, m_socketPath.c_str());
        unlink(m_socketPath.c_str());
        if (bind(m_listen, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(m_listen, 2) != 0) {
            std::perror("lumen-expand-helper: socket");
            return false;
        }
        // Only the configured user may connect.
        if (geteuid() == 0) {
            const passwd *pw = getpwnam(m_user.c_str());
            if (!pw) {
                std::fprintf(stderr, "lumen-expand-helper: no such user '%s' (set user= in the config)\n", m_user.c_str());
                return false;
            }
            if (chown(m_socketPath.c_str(), pw->pw_uid, pw->pw_gid) != 0)
                return false;
        }
        chmod(m_socketPath.c_str(), 0600);
        return true;
    }

    void acceptClient()
    {
        const int fd = accept4(m_listen, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            return;
        if (m_client >= 0)
            close(m_client); // the newest Lumen wins
        m_client = fd;
        m_incoming.clear();
        m_matcher.setKeywords({});
    }

    void readClient()
    {
        char buffer[4096];
        const ssize_t n = read(m_client, buffer, sizeof buffer);
        if (n <= 0) {
            close(m_client);
            m_client = -1;
            m_matcher.setKeywords({}); // nobody to tell: watch for nothing
            return;
        }
        m_incoming.append(buffer, size_t(n));
        size_t end;
        while ((end = m_incoming.find('\n')) != std::string::npos) {
            const std::vector<std::string> parts = split(m_incoming.substr(0, end), '\t');
            m_incoming.erase(0, end + 1);
            if (!parts.empty() && parts[0] == "keywords")
                m_matcher.setKeywords(std::vector<std::string>(parts.begin() + 1, parts.end()));
        }
        if (m_incoming.size() > 65536)
            m_incoming.clear(); // not a well-behaved client
    }

    void handleKey(unsigned code, int value)
    {
        const std::string match = m_matcher.key(code, value);
        if (!match.empty() && m_client >= 0) {
            const std::string line = "match\t" + match + "\n";
            if (write(m_client, line.data(), line.size()) < 0) {
                close(m_client);
                m_client = -1;
            }
        }
    }

    // ---------------------------------------------------------- devices

    void scanDevices()
    {
        DIR *dir = opendir("/dev/input");
        if (!dir)
            return;
        while (const dirent *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind("event", 0) == 0 && !m_devices.count("/dev/input/" + name))
                openDevice("/dev/input/" + name);
        }
        closedir(dir);
    }

    void openDevice(const std::string &path)
    {
        Device d;
        d.fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (d.fd < 0)
            return;
        if (libevdev_new_from_fd(d.fd, &d.dev) != 0) {
            close(d.fd);
            return;
        }
        d.keyboard = libevdev_has_event_code(d.dev, EV_KEY, KEY_A) && libevdev_has_event_code(d.dev, EV_KEY, KEY_Z)
            && libevdev_has_event_code(d.dev, EV_KEY, KEY_SPACE);
        const bool pointer = libevdev_has_event_code(d.dev, EV_KEY, BTN_LEFT);
        if (!d.keyboard && !pointer) {
            closeDevice(d);
            return;
        }
        m_devices[path] = d;
    }

    static void closeDevice(Device &d)
    {
        libevdev_free(d.dev);
        if (d.fd >= 0)
            close(d.fd);
        d = {};
    }

    void deviceFolderChanged()
    {
        char buffer[4096];
        while (read(m_inotify, buffer, sizeof buffer) > 0) {
        }
        // Drop devices that went away, then pick up new ones (udev sets their
        // permissions just after creating them, hence IN_ATTRIB too).
        for (auto it = m_devices.begin(); it != m_devices.end();) {
            if (access(it->first.c_str(), F_OK) != 0) {
                closeDevice(it->second);
                it = m_devices.erase(it);
            } else {
                ++it;
            }
        }
        scanDevices();
    }

    void readDevice(int fd)
    {
        for (auto &[path, d] : m_devices) {
            if (d.fd != fd)
                continue;
            input_event ev;
            int rc;
            while ((rc = libevdev_next_event(d.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0) {
                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    m_matcher.reset(); // events were dropped
                    continue;
                }
                if (ev.type != EV_KEY)
                    continue;
                if (ev.code >= BTN_MISC && ev.code < KEY_OK) {
                    if (ev.value == 1)
                        m_matcher.reset(); // a click may have moved the cursor
                } else if (d.keyboard) {
                    handleKey(ev.code, ev.value);
                }
            }
            if (rc == -ENODEV) {
                closeDevice(d);
                m_devices.erase(path);
            }
            return;
        }
    }

    // Test mode: "code value" pairs, one per line, on standard input.
    bool readTestEvents()
    {
        char buffer[4096];
        const ssize_t n = read(STDIN_FILENO, buffer, sizeof buffer);
        if (n <= 0)
            return false;
        m_testInput.append(buffer, size_t(n));
        size_t end;
        while ((end = m_testInput.find('\n')) != std::string::npos) {
            unsigned code = 0;
            int value = 0;
            if (std::sscanf(m_testInput.substr(0, end).c_str(), "%u %d", &code, &value) == 2)
                handleKey(code, value);
            m_testInput.erase(0, end + 1);
        }
        return true;
    }

    KeyMatcher m_matcher;
    std::string m_socketPath;
    bool m_testEvents;
    std::string m_user;
    int m_listen = -1;
    int m_client = -1;
    int m_inotify = -1;
    std::string m_incoming;
    std::string m_testInput;
    std::map<std::string, Device> m_devices;
};

volatile std::sig_atomic_t stopRequested = 0;

void onSignal(int)
{
    stopRequested = 1;
}

} // namespace

int main(int argc, char **argv)
{
    std::string configPath = "/etc/lumen-expand.conf";
    std::string socketPath = "/run/lumen-expand/socket";
    bool testEvents = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
            configPath = argv[++i];
        else if (arg == "--socket" && i + 1 < argc)
            socketPath = argv[++i];
        else if (arg == "--test-events")
            testEvents = true;
        else {
            std::fprintf(stderr, "usage: lumen-expand-helper [--config FILE] [--socket PATH] [--test-events]\n");
            return 2;
        }
    }
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGPIPE, SIG_IGN); // a vanished client is handled where it is written to

    Helper helper(readConfig(configPath), socketPath, testEvents);
    if (!helper.start())
        return 1;
    helper.run(stopRequested);
    return 0;
}
