#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/thread-watch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" char **environ;

namespace {

constexpr int kDefaultPort = 4444;
constexpr int kMaxEvents = 64;
constexpr int kDefaultDebugPortBase = 5500;
constexpr int kDebugPortRange = 200;
constexpr size_t kMaxOutputBuffer = 256 * 1024;
constexpr const char *kServiceType = "_mydebug._tcp";

int pidfd_open_sys(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

int memfd_create_sys(const char *name, unsigned int flags) {
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
#else
    errno = ENOSYS;
    return -1;
#endif
}

std::string state_to_string(int state) {
    switch (state) {
        case 0: return "LOADED";
        case 1: return "RUNNING";
        case 2: return "DEBUGGING";
        case 3: return "STOPPED";
        default: return "UNKNOWN";
    }
}

struct MdnsAdvertiser {
    AvahiThreadedPoll *poll = nullptr;
    AvahiClient *client = nullptr;
    AvahiEntryGroup *group = nullptr;
    std::string name;
    int port = 0;
    bool started = false;
};

void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    auto *adv = static_cast<MdnsAdvertiser *>(userdata);
    if (!g || !adv) {
        return;
    }
    if (state == AVAHI_ENTRY_GROUP_FAILURE) {
        std::cerr << "mdns: entry group failure: "
                  << avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g)))
                  << "\n";
    }
}

void client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    auto *adv = static_cast<MdnsAdvertiser *>(userdata);
    if (!adv) {
        return;
    }

    if (state != AVAHI_CLIENT_S_RUNNING) {
        return;
    }

    if (!adv->group) {
        adv->group = avahi_entry_group_new(c, entry_group_callback, adv);
    }

    if (!adv->group) {
        std::cerr << "mdns: failed to create entry group\n";
        return;
    }

    if (avahi_entry_group_is_empty(adv->group)) {
        int ret = avahi_entry_group_add_service(
            adv->group,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC,
            AvahiPublishFlags(0),
            adv->name.c_str(),
            kServiceType,
            nullptr,
            nullptr,
            static_cast<uint16_t>(adv->port),
            nullptr);

        if (ret < 0) {
            std::cerr << "mdns: add service failed: " << avahi_strerror(ret) << "\n";
            return;
        }

        ret = avahi_entry_group_commit(adv->group);
        if (ret < 0) {
            std::cerr << "mdns: commit failed: " << avahi_strerror(ret) << "\n";
        }
    }
}

bool start_mdns(MdnsAdvertiser &adv) {
    int error = 0;
    adv.poll = avahi_threaded_poll_new();
    if (!adv.poll) {
        std::cerr << "mdns: failed to create threaded poll\n";
        return false;
    }

    adv.client = avahi_client_new(
        avahi_threaded_poll_get(adv.poll),
        AVAHI_CLIENT_NO_FAIL,
        client_callback,
        &adv,
        &error);

    if (!adv.client) {
        std::cerr << "mdns: client create failed: " << avahi_strerror(error) << "\n";
        avahi_threaded_poll_free(adv.poll);
        adv.poll = nullptr;
        return false;
    }

    if (avahi_threaded_poll_start(adv.poll) < 0) {
        std::cerr << "mdns: failed to start threaded poll\n";
        avahi_client_free(adv.client);
        avahi_threaded_poll_free(adv.poll);
        adv.client = nullptr;
        adv.poll = nullptr;
        return false;
    }

    adv.started = true;
    return true;
}

void stop_mdns(MdnsAdvertiser &adv) {
    if (adv.started && adv.poll) {
        avahi_threaded_poll_stop(adv.poll);
    }
    if (adv.group) {
        avahi_entry_group_free(adv.group);
        adv.group = nullptr;
    }
    if (adv.client) {
        avahi_client_free(adv.client);
        adv.client = nullptr;
    }
    if (adv.poll) {
        avahi_threaded_poll_free(adv.poll);
        adv.poll = nullptr;
    }
    adv.started = false;
}

struct Session {
    std::string id;
    int memfd = -1;
    pid_t pid = -1;
    int debug_port = -1;
    int pidfd = -1;
    pid_t gdb_pid = -1;
    int gdb_pidfd = -1;
    size_t size = 0;
    int state = 0;
    bool is_bundle = false;
    std::string bundle_dir;
    std::string exec_path;
    std::string output;
    int output_pipe_fd = -1;
    std::string saved_args;
    std::map<std::string, std::string> env_vars;
};

struct OutputPipeInfo {
    std::string session_id;
};

struct WatchInfo {
    std::string id;
    bool is_gdb = false;
};

struct ClientConn {
    int fd = -1;
    std::string inbuf;
    bool in_upload = false;
    size_t upload_remaining = 0;
    size_t upload_size = 0;
    int upload_memfd = -1;
    size_t elf_filled = 0;
    unsigned char elf_magic[4] = {0};
    bool is_bundle = false;
    std::string exec_path;
    int upload_tmpfd = -1;
    std::string upload_tmppath;
};

struct Config {
    int port = kDefaultPort;
    int web_port = 0;
    std::string service_name = "debuglantern";
    size_t max_sessions = 32;
    size_t max_total_bytes = 512 * 1024 * 1024ULL;
    int drop_uid = -1;
    int drop_gid = -1;
};

int nftw_remove_cb(const char *fpath, const struct stat * /*sb*/, int /*typeflag*/, struct FTW * /*ftwbuf*/) {
    return remove(fpath);
}

bool remove_directory_recursive(const std::string &path) {
    return nftw(path.c_str(), nftw_remove_cb, 64, FTW_DEPTH | FTW_PHYS) == 0;
}

bool extract_tar_gz(const std::string &archive_path, const std::string &dest_dir) {
    pid_t child = fork();
    if (child == 0) {
        execlp("tar", "tar", "xzf", archive_path.c_str(), "-C", dest_dir.c_str(), nullptr);
        _exit(127);
    }
    if (child < 0) {
        return false;
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct DepStatus {
    std::string name;
    std::string description;
    bool available;
    bool required;
};

std::vector<DepStatus> check_dependencies() {
    std::vector<DepStatus> deps;

    auto check_cmd = [](const char *name) -> bool {
        std::string cmd = "command -v ";
        cmd += name;
        cmd += " >/dev/null 2>&1";
        return system(cmd.c_str()) == 0;
    };

    deps.push_back({"gdbserver", "Required for debug attach and start --debug", check_cmd("gdbserver"), true});
    deps.push_back({"tar", "Required for bundle (tar.gz) extraction", check_cmd("tar"), true});
    deps.push_back({"gzip", "Required for bundle (tar.gz) decompression", check_cmd("gzip"), true});

    return deps;
}

std::string deps_json() {
    auto deps = check_dependencies();
    bool all_ok = true;
    std::ostringstream oss;
    oss << "{\"deps\":[";
    for (size_t i = 0; i < deps.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{" << debuglantern::json_kv("name", deps[i].name, true) << ","
            << debuglantern::json_kv("description", deps[i].description, true) << ","
            << debuglantern::json_kv("available", deps[i].available) << ","
            << debuglantern::json_kv("required", deps[i].required) << "}";
        if (deps[i].required && !deps[i].available) all_ok = false;
    }
    oss << "]," << debuglantern::json_kv("all_satisfied", all_ok) << "}";
    return oss.str();
}

bool validate_elf_file(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    unsigned char magic[4] = {0};
    ssize_t n = read(fd, magic, 4);
    close(fd);
    if (n < 4) {
        return false;
    }
    return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

class Server {
public:
    explicit Server(const Config &cfg)
        : cfg_(cfg) {}

    bool init() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            perror("socket");
            return false;
        }

        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));

        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            perror("bind");
            return false;
        }

        if (listen(listen_fd_, 64) < 0) {
            perror("listen");
            return false;
        }

        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            perror("epoll_create1");
            return false;
        }

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
            perror("epoll_ctl");
            return false;
        }

        return true;
    }

    void loop() {
        std::vector<epoll_event> events(kMaxEvents);
        while (!shutdown_) {
            int n = epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), -1);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("epoll_wait");
                break;
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == listen_fd_) {
                    handle_accept();
                    continue;
                }

                auto watch_it = watches_.find(fd);
                if (watch_it != watches_.end()) {
                    handle_watch(fd, watch_it->second);
                    continue;
                }

                auto pipe_it = output_pipes_.find(fd);
                if (pipe_it != output_pipes_.end()) {
                    handle_output_pipe(fd, pipe_it->second);
                    continue;
                }

                auto conn_it = clients_.find(fd);
                if (conn_it != clients_.end()) {
                    handle_client(conn_it->second);
                    continue;
                }
            }
        }
    }

    void shutdown() {
        shutdown_ = true;
    }

private:
    void handle_accept() {
        while (true) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int fd = accept4(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len, SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("accept");
                break;
            }

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                perror("epoll_ctl client");
                close(fd);
                continue;
            }

            clients_[fd] = ClientConn{fd};
        }
    }

    void close_client(ClientConn &conn) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn.fd, nullptr);
        close(conn.fd);
        if (conn.in_upload && conn.upload_memfd >= 0) {
            close(conn.upload_memfd);
        }
        if (conn.in_upload && conn.upload_tmpfd >= 0) {
            close(conn.upload_tmpfd);
            if (!conn.upload_tmppath.empty()) {
                unlink(conn.upload_tmppath.c_str());
            }
        }
        clients_.erase(conn.fd);
    }

    void handle_client(ClientConn &conn) {
        if (!read_into_buffer(conn)) {
            close_client(conn);
            return;
        }

        if (conn.in_upload) {
            if (!consume_upload(conn)) {
                close_client(conn);
                return;
            }
        }

        while (!conn.in_upload) {
            auto line = read_line(conn.inbuf);
            if (!line.has_value()) {
                break;
            }
            handle_command(conn, *line);
        }

        // Consume any buffered upload data after entering upload mode
        if (conn.in_upload && !conn.inbuf.empty()) {
            if (!consume_upload(conn)) {
                close_client(conn);
                return;
            }
        }
    }

    bool read_into_buffer(ClientConn &conn) {
        char buf[4096];
        while (true) {
            ssize_t n = read(conn.fd, buf, sizeof(buf));
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            }
            conn.inbuf.append(buf, static_cast<size_t>(n));
        }
        return true;
    }

    bool consume_upload(ClientConn &conn) {
        size_t take = std::min(conn.upload_remaining, conn.inbuf.size());
        if (take == 0) {
            return true;
        }

        if (!write_upload_chunk(conn, conn.inbuf.data(), take)) {
            send_error(conn.fd, "upload_write_failed");
            return false;
        }

        conn.inbuf.erase(0, take);
        conn.upload_remaining -= take;

        if (conn.upload_remaining == 0) {
            return finish_upload(conn);
        }

        return true;
    }

    bool write_upload_chunk(ClientConn &conn, const char *data, size_t len) {
        if (!conn.is_bundle && conn.elf_filled < 4) {
            size_t want = std::min(len, static_cast<size_t>(4 - conn.elf_filled));
            for (size_t i = 0; i < want; ++i) {
                conn.elf_magic[conn.elf_filled + i] = static_cast<unsigned char>(data[i]);
            }
            conn.elf_filled += want;
        }

        int write_fd = conn.is_bundle ? conn.upload_tmpfd : conn.upload_memfd;
        size_t off = 0;
        while (off < len) {
            ssize_t wrote = write(write_fd, data + off, len - off);
            if (wrote <= 0) {
                return false;
            }
            off += static_cast<size_t>(wrote);
        }
        return true;
    }

    bool finish_upload(ClientConn &conn) {
        conn.in_upload = false;

        if (conn.is_bundle) {
            return finish_bundle_upload(conn);
        }

        if (conn.elf_filled < 4 || conn.elf_magic[0] != 0x7f || conn.elf_magic[1] != 'E' ||
            conn.elf_magic[2] != 'L' || conn.elf_magic[3] != 'F') {
            send_error(conn.fd, "invalid_elf");
            close(conn.upload_memfd);
            conn.upload_memfd = -1;
            return true;
        }

        if (sessions_.size() >= cfg_.max_sessions) {
            send_error(conn.fd, "max_sessions_reached");
            close(conn.upload_memfd);
            conn.upload_memfd = -1;
            return true;
        }

        if (total_bytes_ + conn.upload_size > cfg_.max_total_bytes) {
            send_error(conn.fd, "max_total_bytes_reached");
            close(conn.upload_memfd);
            conn.upload_memfd = -1;
            return true;
        }

        std::string id = generate_uuid();
        Session s;
        s.id = id;
        s.memfd = conn.upload_memfd;
        s.size = conn.upload_size;
        s.state = 0;

        sessions_[id] = s;
        total_bytes_ += conn.upload_size;

        std::ostringstream oss;
        oss << "{" << debuglantern::json_kv("id", id, true) << ","
            << debuglantern::json_kv("state", state_to_string(s.state), true) << ","
            << debuglantern::json_kv("size", static_cast<long long>(s.size)) << "}\n";
        send_response(conn.fd, oss.str());

        conn.upload_memfd = -1;
        conn.upload_size = 0;
        conn.elf_filled = 0;
        return true;
    }

    bool finish_bundle_upload(ClientConn &conn) {
        close(conn.upload_tmpfd);
        conn.upload_tmpfd = -1;

        if (sessions_.size() >= cfg_.max_sessions) {
            send_error(conn.fd, "max_sessions_reached");
            unlink(conn.upload_tmppath.c_str());
            return true;
        }

        if (total_bytes_ + conn.upload_size > cfg_.max_total_bytes) {
            send_error(conn.fd, "max_total_bytes_reached");
            unlink(conn.upload_tmppath.c_str());
            return true;
        }

        // Create extraction directory
        char tmpdir[] = "/tmp/debuglantern-bundle-XXXXXX";
        if (!mkdtemp(tmpdir)) {
            send_error(conn.fd, "tmpdir_create_failed");
            unlink(conn.upload_tmppath.c_str());
            return true;
        }
        std::string bundle_dir = tmpdir;

        // Extract tar.gz
        if (!extract_tar_gz(conn.upload_tmppath, bundle_dir)) {
            send_error(conn.fd, "extract_failed");
            unlink(conn.upload_tmppath.c_str());
            remove_directory_recursive(bundle_dir);
            return true;
        }

        // Remove the temp archive
        unlink(conn.upload_tmppath.c_str());

        // Validate the exec_path binary exists and is ELF
        std::string full_exec = bundle_dir + "/" + conn.exec_path;
        if (!validate_elf_file(full_exec)) {
            send_error(conn.fd, "invalid_exec_path");
            remove_directory_recursive(bundle_dir);
            return true;
        }

        // Make executable
        chmod(full_exec.c_str(), 0755);

        std::string id = generate_uuid();
        Session s;
        s.id = id;
        s.memfd = -1;
        s.size = conn.upload_size;
        s.state = 0;
        s.is_bundle = true;
        s.bundle_dir = bundle_dir;
        s.exec_path = conn.exec_path;

        sessions_[id] = s;
        total_bytes_ += conn.upload_size;

        std::ostringstream oss;
        oss << "{" << debuglantern::json_kv("id", id, true) << ","
            << debuglantern::json_kv("state", state_to_string(s.state), true) << ","
            << debuglantern::json_kv("size", static_cast<long long>(s.size)) << ","
            << debuglantern::json_kv("bundle", true) << ","
            << debuglantern::json_kv("exec_path", s.exec_path, true) << "}\n";
        send_response(conn.fd, oss.str());

        conn.upload_tmppath.clear();
        conn.upload_size = 0;
        conn.is_bundle = false;
        conn.exec_path.clear();
        conn.elf_filled = 0;
        return true;
    }

    std::optional<std::string> read_line(std::string &buf) {
        size_t pos = buf.find('\n');
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    }

    void handle_command(ClientConn &conn, const std::string &line) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "UPLOAD") {
            size_t size = 0;
            iss >> size;
            if (size == 0) {
                send_error(conn.fd, "invalid_size");
                return;
            }
            if (conn.in_upload) {
                send_error(conn.fd, "upload_in_progress");
                return;
            }

            std::string exec_path;
            iss >> exec_path;
            bool is_bundle = !exec_path.empty();

            if (is_bundle) {
                // Validate exec_path doesn't escape the bundle
                if (exec_path.find("..") != std::string::npos) {
                    send_error(conn.fd, "invalid_exec_path");
                    return;
                }

                // Create temp file for tar.gz
                char tmppath[] = "/tmp/debuglantern-upload-XXXXXX";
                int tmpfd = mkstemp(tmppath);
                if (tmpfd < 0) {
                    send_error(conn.fd, "tmpfile_create_failed");
                    return;
                }

                conn.in_upload = true;
                conn.upload_remaining = size;
                conn.upload_size = size;
                conn.is_bundle = true;
                conn.exec_path = exec_path;
                conn.upload_tmpfd = tmpfd;
                conn.upload_tmppath = tmppath;
                conn.upload_memfd = -1;
                conn.elf_filled = 0;
            } else {
                int memfd = memfd_create_sys("debuglantern", MFD_CLOEXEC);
                if (memfd < 0) {
                    send_error(conn.fd, "memfd_create_failed");
                    return;
                }

                conn.in_upload = true;
                conn.upload_remaining = size;
                conn.upload_size = size;
                conn.upload_memfd = memfd;
                conn.is_bundle = false;
                conn.exec_path.clear();
                conn.upload_tmpfd = -1;
                conn.upload_tmppath.clear();
                conn.elf_filled = 0;
            }
            return;
        }

        if (cmd == "LIST") {
            send_list(conn.fd);
            return;
        }

        if (cmd == "DEPS") {
            std::string json = deps_json();
            json += "\n";
            send_response(conn.fd, json);
            return;
        }

        if (cmd == "OUTPUT") {
            std::string id;
            iss >> id;
            size_t offset = 0;
            std::string off_str;
            if (iss >> off_str) {
                offset = std::stoull(off_str);
            }
            handle_output(conn.fd, id, offset);
            return;
        }

        if (cmd == "STATUS") {
            std::string id;
            iss >> id;
            send_status(conn.fd, id);
            return;
        }

        if (cmd == "ARGS") {
            std::string id;
            iss >> id;
            std::string rest;
            std::getline(iss, rest);
            // Trim leading space
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            handle_set_args(conn.fd, id, rest);
            return;
        }

        if (cmd == "ENV") {
            std::string id;
            iss >> id;
            std::string rest;
            std::getline(iss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            handle_set_env(conn.fd, id, rest);
            return;
        }

        if (cmd == "ENVDEL") {
            std::string id, key;
            iss >> id >> key;
            handle_del_env(conn.fd, id, key);
            return;
        }

        if (cmd == "ENVLIST") {
            std::string id;
            iss >> id;
            handle_list_env(conn.fd, id);
            return;
        }

        if (cmd == "START") {
            std::string id;
            iss >> id;
            bool debug = false;
            std::string token;
            while (iss >> token) {
                if (token == "--debug") {
                    debug = true;
                }
            }
            handle_start(conn.fd, id, debug);
            return;
        }

        if (cmd == "STOP") {
            std::string id;
            iss >> id;
            handle_stop(conn.fd, id, SIGTERM);
            return;
        }

        if (cmd == "KILL") {
            std::string id;
            iss >> id;
            handle_stop(conn.fd, id, SIGKILL);
            return;
        }

        if (cmd == "DEBUG") {
            std::string id;
            iss >> id;
            handle_debug(conn.fd, id);
            return;
        }

        if (cmd == "DELETE") {
            std::string id;
            iss >> id;
            handle_delete(conn.fd, id);
            return;
        }

        if (cmd == "SYSROOT") {
            handle_sysroot(conn.fd);
            return;
        }

        send_error(conn.fd, "unknown_command");
    }

    void handle_set_env(int fd, const std::string &id, const std::string &kv) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }
        auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            send_error(fd, "invalid_env");
            return;
        }
        std::string key = kv.substr(0, eq);
        std::string val = kv.substr(eq + 1);
        it->second.env_vars[key] = val;
        send_status(fd, id);
    }

    void handle_del_env(int fd, const std::string &id, const std::string &key) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }
        it->second.env_vars.erase(key);
        send_status(fd, id);
    }

    void handle_list_env(int fd, const std::string &id) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto &kv : it->second.env_vars) {
            if (!first) oss << ",";
            oss << debuglantern::json_kv(kv.first, kv.second, true);
            first = false;
        }
        oss << "}\n";
        send_response(fd, oss.str());
    }

    static std::vector<std::string> build_env(const std::map<std::string, std::string> &overrides) {
        std::map<std::string, std::string> merged;
        // Start with current environ
        for (char **e = environ; *e; ++e) {
            std::string entry(*e);
            auto eq = entry.find('=');
            if (eq != std::string::npos) {
                merged[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
        }
        // Apply overrides
        for (const auto &kv : overrides) {
            merged[kv.first] = kv.second;
        }
        std::vector<std::string> result;
        result.reserve(merged.size());
        for (const auto &kv : merged) {
            result.push_back(kv.first + "=" + kv.second);
        }
        return result;
    }

    static std::vector<char *> env_ptrs(std::vector<std::string> &env_strs) {
        std::vector<char *> ptrs;
        ptrs.reserve(env_strs.size() + 1);
        for (auto &s : env_strs) {
            ptrs.push_back(s.data());
        }
        ptrs.push_back(nullptr);
        return ptrs;
    }

    void handle_set_args(int fd, const std::string &id, const std::string &args) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }
        it->second.saved_args = args;
        send_status(fd, id);
    }

    static std::vector<std::string> split_args(const std::string &s) {
        std::vector<std::string> result;
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) {
            result.push_back(tok);
        }
        return result;
    }

    void handle_start(int fd, const std::string &id, bool debug) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }

        Session &s = it->second;
        if (s.state == 1 || s.state == 2) {
            send_error(fd, "already_running");
            return;
        }

        // Clear previous output
        s.output.clear();

        auto args = split_args(s.saved_args);
        auto env_strs = build_env(s.env_vars);
        auto envp = env_ptrs(env_strs);

        if (s.is_bundle) {
            handle_start_bundle(fd, s, debug, args, envp);
            return;
        }

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            send_error(fd, "fork_failed");
            return;
        }

        if (debug) {
            int port = alloc_debug_port();
            pid_t child = fork();
            if (child == 0) {
                setpgid(0, 0);
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
                std::string fdpath = "/proc/self/fd/" + std::to_string(s.memfd);
                std::string port_arg = ":" + std::to_string(port);
                std::vector<char *> argv_vec;
                argv_vec.push_back(const_cast<char *>("gdbserver"));
                argv_vec.push_back(const_cast<char *>(port_arg.c_str()));
                argv_vec.push_back(const_cast<char *>(fdpath.c_str()));
                for (const auto &a : args) {
                    argv_vec.push_back(const_cast<char *>(a.c_str()));
                }
                argv_vec.push_back(nullptr);
                execvpe("gdbserver", argv_vec.data(), envp.data());
                _exit(127);
            }
            if (child < 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                send_error(fd, "fork_failed");
                return;
            }

            setpgid(child, child);
            close(pipefd[1]);
            s.pid = child;
            s.gdb_pid = child;
            s.debug_port = port;
            s.state = 2;
            setup_output_pipe(s, pipefd[0]);
            add_watch(child, s.id, true);
            send_status(fd, s.id);
            return;
        }

        pid_t child = fork();
        if (child == 0) {
            setpgid(0, 0);
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
            std::string path = "/proc/self/fd/" + std::to_string(s.memfd);
            std::vector<char *> argv_vec;
            argv_vec.push_back(const_cast<char *>(path.c_str()));
            for (const auto &a : args) {
                argv_vec.push_back(const_cast<char *>(a.c_str()));
            }
            argv_vec.push_back(nullptr);
            fexecve(s.memfd, argv_vec.data(), envp.data());
            _exit(127);
        }

        if (child < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            send_error(fd, "fork_failed");
            return;
        }

        setpgid(child, child);
        close(pipefd[1]);
        s.pid = child;
        s.state = 1;
        setup_output_pipe(s, pipefd[0]);
        add_watch(child, s.id, false);
        send_status(fd, s.id);
    }

    void handle_start_bundle(int fd, Session &s, bool debug,
                             const std::vector<std::string> &args,
                             std::vector<char *> &envp) {
        std::string full_exec = s.bundle_dir + "/" + s.exec_path;

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            send_error(fd, "fork_failed");
            return;
        }

        if (debug) {
            int port = alloc_debug_port();
            pid_t child = fork();
            if (child == 0) {
                setpgid(0, 0);
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
                if (chdir(s.bundle_dir.c_str()) != 0) {
                    _exit(127);
                }
                std::string port_arg = ":" + std::to_string(port);
                std::vector<char *> argv_vec;
                argv_vec.push_back(const_cast<char *>("gdbserver"));
                argv_vec.push_back(const_cast<char *>(port_arg.c_str()));
                argv_vec.push_back(const_cast<char *>(full_exec.c_str()));
                for (const auto &a : args) {
                    argv_vec.push_back(const_cast<char *>(a.c_str()));
                }
                argv_vec.push_back(nullptr);
                execvpe("gdbserver", argv_vec.data(), envp.data());
                _exit(127);
            }
            if (child < 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                send_error(fd, "fork_failed");
                return;
            }

            setpgid(child, child);
            close(pipefd[1]);
            s.pid = child;
            s.gdb_pid = child;
            s.debug_port = port;
            s.state = 2;
            setup_output_pipe(s, pipefd[0]);
            add_watch(child, s.id, true);
            send_status(fd, s.id);
            return;
        }

        pid_t child = fork();
        if (child == 0) {
            setpgid(0, 0);
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
            if (chdir(s.bundle_dir.c_str()) != 0) {
                _exit(127);
            }
            std::vector<char *> argv_vec;
            argv_vec.push_back(const_cast<char *>(full_exec.c_str()));
            for (const auto &a : args) {
                argv_vec.push_back(const_cast<char *>(a.c_str()));
            }
            argv_vec.push_back(nullptr);
            execve(full_exec.c_str(), argv_vec.data(), envp.data());
            _exit(127);
        }

        if (child < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            send_error(fd, "fork_failed");
            return;
        }

        setpgid(child, child);
        close(pipefd[1]);
        s.pid = child;
        s.state = 1;
        setup_output_pipe(s, pipefd[0]);
        add_watch(child, s.id, false);
        send_status(fd, s.id);
    }

    void setup_output_pipe(Session &s, int read_fd) {
        debuglantern::set_nonblocking(read_fd);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = read_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, read_fd, &ev) < 0) {
            close(read_fd);
            return;
        }
        s.output_pipe_fd = read_fd;
        output_pipes_[read_fd] = OutputPipeInfo{s.id};
    }

    void close_output_pipe(Session &s) {
        if (s.output_pipe_fd >= 0) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, s.output_pipe_fd, nullptr);
            close(s.output_pipe_fd);
            output_pipes_.erase(s.output_pipe_fd);
            s.output_pipe_fd = -1;
        }
    }

    void handle_output_pipe(int pipefd, const OutputPipeInfo &info) {
        char buf[4096];
        ssize_t n = read(pipefd, buf, sizeof(buf));
        if (n <= 0) {
            // Pipe closed
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, pipefd, nullptr);
            close(pipefd);
            auto it = sessions_.find(info.session_id);
            if (it != sessions_.end() && it->second.output_pipe_fd == pipefd) {
                it->second.output_pipe_fd = -1;
            }
            output_pipes_.erase(pipefd);
            return;
        }

        auto it = sessions_.find(info.session_id);
        if (it != sessions_.end()) {
            it->second.output.append(buf, static_cast<size_t>(n));
            if (it->second.output.size() > kMaxOutputBuffer) {
                it->second.output.erase(0,
                    it->second.output.size() - kMaxOutputBuffer);
            }
        }
    }

    void handle_output(int fd, const std::string &id, size_t offset) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }

        const Session &s = it->second;
        std::string data;
        if (offset < s.output.size()) {
            data = s.output.substr(offset);
        }

        std::ostringstream oss;
        oss << "{" << debuglantern::json_kv("id", s.id, true) << ","
            << debuglantern::json_kv("output", data, true) << ","
            << debuglantern::json_kv("offset", static_cast<long long>(offset)) << ","
            << debuglantern::json_kv("total", static_cast<long long>(s.output.size()))
            << "}\n";
        send_response(fd, oss.str());
    }

    void handle_stop(int fd, const std::string &id, int sig) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }

        Session &s = it->second;
        if (s.pid <= 0) {
            send_error(fd, "not_running");
            return;
        }

        // Kill the entire process group first, then the leader.
        // Group kill may already terminate the leader, so ignore
        // errors on the individual kill.
        kill(-s.pid, sig);
        kill(s.pid, sig);

        // For SIGKILL, try to reap immediately so the state
        // transitions even if the pidfd watch hasn't fired yet
        // (e.g. process stuck in D state / DRM uninterruptible sleep).
        if (sig == SIGKILL) {
            force_reap(s);
        }

        send_status(fd, s.id);
    }

    // Attempt to reap the process immediately and update session state.
    // Handles both plain and debug (gdbserver-wrapped) sessions.
    void force_reap(Session &s) {
        // Try to reap the main pid
        if (s.pid > 0) {
            int status = 0;
            pid_t w = waitpid(s.pid, &status, WNOHANG);
            if (w > 0 || (w < 0 && errno == ECHILD)) {
                // Process is dead or not our child; clean up state
                if (s.pidfd >= 0) {
                    cleanup_watch(s.pidfd);
                    s.pidfd = -1;
                }
                if (s.gdb_pidfd >= 0 && s.gdb_pidfd != s.pidfd) {
                    cleanup_watch(s.gdb_pidfd);
                    s.gdb_pidfd = -1;
                }
                close_output_pipe(s);
                s.pid = -1;
                s.gdb_pid = -1;
                s.debug_port = -1;
                s.state = 3;
                return;
            }
        }

        // If gdbserver is a separate process, try reaping it too
        if (s.gdb_pid > 0 && s.gdb_pid != s.pid) {
            int status = 0;
            pid_t w = waitpid(s.gdb_pid, &status, WNOHANG);
            if (w > 0 || (w < 0 && errno == ECHILD)) {
                if (s.gdb_pidfd >= 0) {
                    cleanup_watch(s.gdb_pidfd);
                    s.gdb_pidfd = -1;
                }
                s.gdb_pid = -1;
                s.debug_port = -1;
                if (s.state == 2) {
                    s.state = (s.pid > 0) ? 1 : 3;
                }
            }
        }
    }

    void handle_debug(int fd, const std::string &id) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }

        Session &s = it->second;
        if (s.state != 1) {
            send_error(fd, "not_running");
            return;
        }

        int port = alloc_debug_port();
        pid_t child = fork();
        if (child == 0) {
            std::string port_arg = ":" + std::to_string(port);
            std::string pid_arg = std::to_string(s.pid);
            execlp("gdbserver", "gdbserver", port_arg.c_str(), "--attach", pid_arg.c_str(), nullptr);
            _exit(127);
        }

        if (child < 0) {
            send_error(fd, "fork_failed");
            return;
        }

        s.gdb_pid = child;
        s.debug_port = port;
        s.state = 2;
        add_watch(child, s.id, true);
        send_status(fd, s.id);
    }

    void handle_delete(int fd, const std::string &id) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }

        Session &s = it->second;
        if (s.state == 1 || s.state == 2) {
            send_error(fd, "session_running");
            return;
        }

        if (s.memfd >= 0) {
            close(s.memfd);
            s.memfd = -1;
        }
        close_output_pipe(s);
        if (s.is_bundle && !s.bundle_dir.empty()) {
            remove_directory_recursive(s.bundle_dir);
        }
        total_bytes_ -= s.size;
        sessions_.erase(it);

        std::ostringstream oss;
        oss << "{" << debuglantern::json_kv("id", id, true) << ","
            << debuglantern::json_kv("state", "DELETED", true) << "}\n";
        send_response(fd, oss.str());
    }

    void send_list(int fd) {
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const auto &kv : sessions_) {
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << session_json(kv.second);
        }
        oss << "]\n";
        send_response(fd, oss.str());
    }

    void send_status(int fd, const std::string &id) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            send_error(fd, "not_found");
            return;
        }
        std::ostringstream oss;
        oss << session_json(it->second) << "\n";
        send_response(fd, oss.str());
    }

    std::string session_json(const Session &s) const {
        std::ostringstream oss;
        oss << "{";
        oss << debuglantern::json_kv("id", s.id, true) << ",";
        oss << debuglantern::json_kv("state", state_to_string(s.state), true) << ",";
        if (s.pid > 0) {
            oss << debuglantern::json_kv("pid", static_cast<long long>(s.pid)) << ",";
        } else {
            oss << debuglantern::json_kv("pid", "null", false) << ",";
        }
        if (s.debug_port > 0) {
            oss << debuglantern::json_kv("debug_port", static_cast<long long>(s.debug_port));
        } else {
            oss << debuglantern::json_kv("debug_port", "null", false);
        }
        if (s.is_bundle) {
            oss << "," << debuglantern::json_kv("bundle", true);
            oss << "," << debuglantern::json_kv("exec_path", s.exec_path, true);
            oss << "," << debuglantern::json_kv("bundle_dir", s.bundle_dir, true);
        }
        if (!s.saved_args.empty()) {
            oss << "," << debuglantern::json_kv("args", s.saved_args, true);
        }
        if (!s.env_vars.empty()) {
            oss << ",\"env\":{";
            bool first = true;
            for (const auto &kv : s.env_vars) {
                if (!first) oss << ",";
                oss << debuglantern::json_kv(kv.first, kv.second, true);
                first = false;
            }
            oss << "}";
        }
        oss << "}";
        return oss.str();
    }

    std::string error_message(const std::string &code) const {
        if (code == "invalid_size") return "upload size must be > 0";
        if (code == "upload_in_progress") return "upload already in progress";
        if (code == "memfd_create_failed") return "memfd_create failed";
        if (code == "upload_write_failed") return "failed to write upload data";
        if (code == "invalid_elf") return "uploaded file is not a valid ELF";
        if (code == "max_sessions_reached") return "maximum session count reached";
        if (code == "max_total_bytes_reached") return "maximum total RAM usage reached";
        if (code == "not_found") return "session not found";
        if (code == "already_running") return "session is already running";
        if (code == "not_running") return "session is not running";
        if (code == "fork_failed") return "fork failed";
        if (code == "kill_failed") return "failed to signal process";
        if (code == "session_running") return "session must be stopped before delete";
        if (code == "unknown_command") return "unknown command";
        if (code == "invalid_exec_path") return "exec_path not found or not a valid ELF in bundle";
        if (code == "tmpfile_create_failed") return "failed to create temporary file";
        if (code == "tmpdir_create_failed") return "failed to create temporary directory";
        if (code == "extract_failed") return "failed to extract tar.gz bundle";
        if (code == "invalid_env") return "env format must be KEY=VALUE";
        if (code == "sysroot_tmpfile_failed") return "failed to create temp file for sysroot";
        if (code == "sysroot_no_libs") return "no lib directories found on host";
        if (code == "sysroot_tar_failed") return "failed to create sysroot tarball";
        return "unspecified error";
    }

    void send_error(int fd, const std::string &err) {
        std::ostringstream oss;
        oss << "{" << debuglantern::json_kv("ok", false) << ","
            << debuglantern::json_kv("error_code", err, true) << ","
            << debuglantern::json_kv("message", error_message(err), true) << ","
            << debuglantern::json_kv("time", debuglantern::now_iso8601(), true) << "}\n";
        send_response(fd, oss.str());
    }

    void handle_sysroot(int fd) {
        // Create temp file for the sysroot tarball
        char tmppath[] = "/tmp/debuglantern-sysroot-XXXXXX";
        int tmpfd = mkstemp(tmppath);
        if (tmpfd < 0) {
            send_error(fd, "sysroot_tmpfile_failed");
            return;
        }
        close(tmpfd);

        // Build tar command — collect lib dirs that exist
        std::vector<std::string> dirs;
        struct stat st;
        if (::stat("/lib", &st) == 0) dirs.push_back("/lib");
        if (::stat("/lib64", &st) == 0) dirs.push_back("/lib64");
        if (::stat("/usr/lib", &st) == 0) dirs.push_back("/usr/lib");
        if (::stat("/usr/lib/debug", &st) == 0) dirs.push_back("/usr/lib/debug");

        if (dirs.empty()) {
            unlink(tmppath);
            send_error(fd, "sysroot_no_libs");
            return;
        }

        // Build tar command: tar czf <tmppath> --dereference <dirs...>
        std::string tar_cmd = "tar czf ";
        tar_cmd += tmppath;
        tar_cmd += " --dereference";
        for (const auto &d : dirs) {
            tar_cmd += " " + d;
        }
        tar_cmd += " 2>/dev/null";

        int ret = system(tar_cmd.c_str());
        if (ret != 0) {
            // tar may return non-zero for permission errors but still produce output
            struct stat check;
            if (::stat(tmppath, &check) != 0 || check.st_size == 0) {
                unlink(tmppath);
                send_error(fd, "sysroot_tar_failed");
                return;
            }
        }

        // Get size of tarball
        struct stat tarstat;
        if (::stat(tmppath, &tarstat) != 0 || tarstat.st_size == 0) {
            unlink(tmppath);
            send_error(fd, "sysroot_tar_failed");
            return;
        }

        size_t size = static_cast<size_t>(tarstat.st_size);

        // Send header: SYSROOT <size>\n
        std::string header = "SYSROOT " + std::to_string(size) + "\n";
        ssize_t hw = write(fd, header.data(), header.size());
        if (hw != static_cast<ssize_t>(header.size())) {
            unlink(tmppath);
            return;
        }

        // Stream the tarball to the client
        int tfd = open(tmppath, O_RDONLY);
        if (tfd < 0) {
            unlink(tmppath);
            return;
        }

        char buf[65536];
        size_t remaining = size;
        while (remaining > 0) {
            size_t chunk = std::min(remaining, sizeof(buf));
            ssize_t nr = read(tfd, buf, chunk);
            if (nr <= 0) break;
            size_t off = 0;
            while (off < static_cast<size_t>(nr)) {
                ssize_t nw = write(fd, buf + off, static_cast<size_t>(nr) - off);
                if (nw <= 0) {
                    close(tfd);
                    unlink(tmppath);
                    return;
                }
                off += static_cast<size_t>(nw);
            }
            remaining -= static_cast<size_t>(nr);
        }

        close(tfd);
        unlink(tmppath);
    }

    void send_response(int fd, const std::string &payload) {
        ssize_t n = write(fd, payload.data(), payload.size());
        (void)n;
    }

    std::string generate_uuid() {
        uuid_t uuid;
        uuid_generate(uuid);
        char out[37] = {0};
        uuid_unparse_lower(uuid, out);
        return std::string(out);
    }

    void add_watch(pid_t pid, const std::string &id, bool is_gdb) {
        int pidfd = pidfd_open_sys(pid);
        if (pidfd < 0) {
            return;
        }
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = pidfd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, pidfd, &ev) < 0) {
            close(pidfd);
            return;
        }
        watches_[pidfd] = WatchInfo{id, is_gdb};

        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            if (is_gdb) {
                it->second.gdb_pidfd = pidfd;
            } else {
                it->second.pidfd = pidfd;
            }
        }
    }

    void handle_watch(int pidfd, const WatchInfo &watch) {
        auto it = sessions_.find(watch.id);
        if (it == sessions_.end()) {
            cleanup_watch(pidfd);
            return;
        }

        Session &s = it->second;
        bool gdb_is_app = watch.is_gdb && s.pid == s.gdb_pid && s.pid > 0;
        pid_t pid = watch.is_gdb ? s.gdb_pid : s.pid;
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, WNOHANG);
        }

        if (watch.is_gdb) {
            if (gdb_is_app && pid > 0) {
                kill(-pid, SIGKILL);
            }
            s.gdb_pid = -1;
            s.debug_port = -1;
            if (s.state == 2) {
                if (gdb_is_app) {
                    s.state = 3;
                    s.pid = -1;
                } else if (s.pid > 0) {
                    s.state = 1;
                } else {
                    s.state = 3;
                }
            }
        } else {
            s.pid = -1;
            s.state = 3;
        }

        cleanup_watch(pidfd);
    }

    void cleanup_watch(int pidfd) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, pidfd, nullptr);
        close(pidfd);
        watches_.erase(pidfd);
    }

    int alloc_debug_port() {
        int port = debug_port_next_;
        debug_port_next_++;
        if (debug_port_next_ >= kDefaultDebugPortBase + kDebugPortRange) {
            debug_port_next_ = kDefaultDebugPortBase;
        }
        return port;
    }

    Config cfg_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    bool shutdown_ = false;

    std::unordered_map<int, ClientConn> clients_;
    std::unordered_map<int, WatchInfo> watches_;
    std::unordered_map<int, OutputPipeInfo> output_pipes_;
    std::unordered_map<std::string, Session> sessions_;
    size_t total_bytes_ = 0;
    int debug_port_next_ = kDefaultDebugPortBase;
};

void usage() {
    std::cout << "debuglanternd --port 4444 --web-port 8080 --service-name debuglantern "
                 "--max-sessions 32 --max-total-bytes 536870912 --uid 0 --gid 0\n";
}

Config parse_args(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            cfg.port = std::atoi(argv[++i]);
        } else if (arg == "--web-port" && i + 1 < argc) {
            cfg.web_port = std::atoi(argv[++i]);
        } else if (arg == "--service-name" && i + 1 < argc) {
            cfg.service_name = argv[++i];
        } else if (arg == "--max-sessions" && i + 1 < argc) {
            cfg.max_sessions = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--max-total-bytes" && i + 1 < argc) {
            cfg.max_total_bytes = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--uid" && i + 1 < argc) {
            cfg.drop_uid = std::atoi(argv[++i]);
        } else if (arg == "--gid" && i + 1 < argc) {
            cfg.drop_gid = std::atoi(argv[++i]);
        } else if (arg == "--help") {
            usage();
            std::exit(0);
        }
    }
    return cfg;
}

void drop_privs(const Config &cfg) {
    if (cfg.drop_gid >= 0) {
        if (setgid(static_cast<gid_t>(cfg.drop_gid)) != 0) {
            perror("setgid");
        }
    }
    if (cfg.drop_uid >= 0) {
        if (setuid(static_cast<uid_t>(cfg.drop_uid)) != 0) {
            perror("setuid");
        }
    }
}

}  // namespace

#include "webui.h"

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    Config cfg = parse_args(argc, argv);

    MdnsAdvertiser adv;
    adv.name = cfg.service_name;
    adv.port = cfg.port;
    if (!start_mdns(adv)) {
        std::cerr << "mdns: disabled\n";
    }

    drop_privs(cfg);

    Server server(cfg);
    if (!server.init()) {
        stop_mdns(adv);
        return 1;
    }

    std::unique_ptr<debuglantern::WebUI> webui;
    if (cfg.web_port > 0) {
        webui = std::make_unique<debuglantern::WebUI>(cfg.web_port, cfg.port);
        if (webui->start()) {
            std::cout << "webui: http://0.0.0.0:" << cfg.web_port << "\n";
        } else {
            std::cerr << "webui: failed to start on port " << cfg.web_port << "\n";
            webui.reset();
        }
    }

    std::cout << "debuglanternd listening on port " << cfg.port << "\n";
    server.loop();

    if (webui) webui->stop();
    stop_mdns(adv);
    return 0;
}
