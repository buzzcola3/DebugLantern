#include "common.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Target {
    std::string host = "127.0.0.1";
    int port = 4444;
};

void usage() {
    std::cout << "debuglanternctl <cmd> [args] --target host --port 4444\n"
                 "commands: upload <file> [--exec-path <path>], start <id> [--debug], stop <id>,\n"
                 "          kill <id>, debug <id>, list, status <id>, delete <id>, deps\n"
                 "\n"
                 "  --exec-path  path to binary inside a tar.gz bundle (triggers bundle upload)\n";
}

Target parse_target(int &argc, char **argv) {
    Target t;
    std::vector<char *> out;
    out.reserve(argc);
    out.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--target" && i + 1 < argc) {
            t.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            t.port = std::atoi(argv[++i]);
        } else {
            out.push_back(argv[i]);
        }
    }

    argc = static_cast<int>(out.size());
    for (int i = 0; i < argc; ++i) {
        argv[i] = out[i];
    }
    return t;
}

int connect_to(const Target &t) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    int rc = getaddrinfo(t.host.c_str(), std::to_string(t.port).c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n";
        return -1;
    }

    int fd = -1;
    for (addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

bool read_all(int fd, std::string &out) {
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        out.append(buf, static_cast<size_t>(n));
        if (out.find('\n') != std::string::npos) {
            break;
        }
    }
    return true;
}

bool send_line(int fd, const std::string &line) {
    std::string payload = line + "\n";
    ssize_t n = write(fd, payload.data(), payload.size());
    return n == static_cast<ssize_t>(payload.size());
}

bool send_file(int fd, const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "failed to open file\n";
        return false;
    }

    std::vector<char> buf(4096);
    while (in) {
        in.read(buf.data(), buf.size());
        std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        size_t off = 0;
        while (off < static_cast<size_t>(got)) {
            ssize_t n = write(fd, buf.data() + off, static_cast<size_t>(got) - off);
            if (n <= 0) {
                return false;
            }
            off += static_cast<size_t>(n);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    Target target = parse_target(argc, argv);

    std::string cmd = argv[1];
    int fd = connect_to(target);
    if (fd < 0) {
        std::cerr << "failed to connect\n";
        return 1;
    }

    if (cmd == "upload") {
        if (argc < 3) {
            usage();
            return 1;
        }
        std::string filepath = argv[2];
        std::string exec_path;

        // Parse --exec-path from remaining args
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--exec-path" && i + 1 < argc) {
                exec_path = argv[++i];
            }
        }

        size_t size = 0;
        struct stat st{};
        if (stat(filepath.c_str(), &st) != 0) {
            perror("stat");
            return 1;
        }
        size = static_cast<size_t>(st.st_size);

        std::string upload_cmd = "UPLOAD " + std::to_string(size);
        if (!exec_path.empty()) {
            upload_cmd += " " + exec_path;
        }

        if (!send_line(fd, upload_cmd)) {
            return 1;
        }
        if (!send_file(fd, filepath)) {
            std::cerr << "upload failed\n";
            return 1;
        }
    } else {
        std::ostringstream oss;
        std::string verb = argv[1];
        std::transform(verb.begin(), verb.end(), verb.begin(), ::toupper);
        oss << verb;
        for (int i = 2; i < argc; ++i) {
            oss << " " << argv[i];
        }
        if (!send_line(fd, oss.str())) {
            std::cerr << "send failed\n";
            return 1;
        }
    }

    std::string resp;
    if (!read_all(fd, resp)) {
        std::cerr << "read failed\n";
        return 1;
    }

    std::cout << resp;
    close(fd);
    return 0;
}
