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
                 "commands: upload <file> [--exec-path <path>],\n"
                 "          args <id> \"arg1 arg2 ...\", start <id> [--debug],\n"
                 "          env <id> KEY=VALUE, envdel <id> KEY, envlist <id>,\n"
                 "          stop <id>, kill <id>, debug <id>, list, status <id>, delete <id>,\n"
                 "          output <id> [--follow], deps,\n"
                 "          sysroot <dest-dir>\n"
                 "\n"
                 "  --exec-path       path to binary inside a tar.gz bundle (triggers bundle upload)\n"
                 "  args <id> \"...\"  set arguments for a session (saved, used on every start)\n"
                 "  env <id> K=V      set an environment variable for a session\n"
                 "  envdel <id> KEY   remove an environment variable\n"
                 "  envlist <id>      list environment variables for a session\n"
                 "  --follow          continuously stream output (for output command)\n"
                 "  sysroot <dir>     download device /lib, /lib64, /usr/lib into <dir>\n";
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
    } else if (cmd == "output") {
        if (argc < 3) {
            usage();
            return 1;
        }
        std::string id = argv[2];
        bool follow = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--follow") {
                follow = true;
            }
        }

        if (!follow) {
            if (!send_line(fd, "OUTPUT " + id)) {
                std::cerr << "send failed\n";
                return 1;
            }
            std::string resp;
            if (!read_all(fd, resp)) {
                std::cerr << "read failed\n";
                return 1;
            }
            // Parse and print just the output content
            auto ostart = resp.find("\"output\":\"");
            if (ostart != std::string::npos) {
                ostart += 10;
                auto oend = resp.find("\"", ostart);
                if (oend != std::string::npos) {
                    std::string out = resp.substr(ostart, oend - ostart);
                    // Unescape JSON string
                    std::string decoded;
                    for (size_t i = 0; i < out.size(); ++i) {
                        if (out[i] == '\\' && i + 1 < out.size()) {
                            char c = out[i + 1];
                            if (c == 'n') { decoded += '\n'; ++i; }
                            else if (c == 'r') { decoded += '\r'; ++i; }
                            else if (c == 't') { decoded += '\t'; ++i; }
                            else if (c == '\\') { decoded += '\\'; ++i; }
                            else if (c == '"') { decoded += '"'; ++i; }
                            else { decoded += out[i]; }
                        } else {
                            decoded += out[i];
                        }
                    }
                    std::cout << decoded;
                }
            }
            close(fd);
            return 0;
        }

        // Follow mode: poll output with offset
        size_t offset = 0;
        while (true) {
            int cfd = connect_to(target);
            if (cfd < 0) {
                usleep(500000);
                continue;
            }
            if (!send_line(cfd, "OUTPUT " + id + " " + std::to_string(offset))) {
                close(cfd);
                usleep(500000);
                continue;
            }
            std::string resp;
            if (!read_all(cfd, resp)) {
                close(cfd);
                usleep(500000);
                continue;
            }
            close(cfd);

            // Parse total from response
            auto tstart = resp.find("\"total\":");
            size_t total = offset;
            if (tstart != std::string::npos) {
                tstart += 8;
                total = std::stoull(resp.substr(tstart));
            }

            // Parse and print output
            auto ostart = resp.find("\"output\":\"");
            if (ostart != std::string::npos) {
                ostart += 10;
                auto oend = resp.find("\"", ostart);
                if (oend != std::string::npos) {
                    std::string out = resp.substr(ostart, oend - ostart);
                    std::string decoded;
                    for (size_t i = 0; i < out.size(); ++i) {
                        if (out[i] == '\\' && i + 1 < out.size()) {
                            char c = out[i + 1];
                            if (c == 'n') { decoded += '\n'; ++i; }
                            else if (c == 'r') { decoded += '\r'; ++i; }
                            else if (c == 't') { decoded += '\t'; ++i; }
                            else if (c == '\\') { decoded += '\\'; ++i; }
                            else if (c == '"') { decoded += '"'; ++i; }
                            else { decoded += out[i]; }
                        } else {
                            decoded += out[i];
                        }
                    }
                    if (!decoded.empty()) {
                        std::cout << decoded << std::flush;
                    }
                }
            }

            offset = total;
            usleep(500000);
        }
    } else if (cmd == "sysroot") {
        if (argc < 3) {
            std::cerr << "usage: debuglanternctl sysroot <dest-dir>\n";
            return 1;
        }
        std::string dest_dir = argv[2];

        // Create dest dir if needed
        mkdir(dest_dir.c_str(), 0755);

        if (!send_line(fd, "SYSROOT")) {
            std::cerr << "send failed\n";
            return 1;
        }

        // Read response header line: "SYSROOT <size>\n" or error JSON
        std::string header;
        if (!read_all(fd, header)) {
            std::cerr << "read failed\n";
            return 1;
        }

        // Check for error response
        if (header.find("\"ok\":false") != std::string::npos) {
            std::cout << header;
            close(fd);
            return 1;
        }

        // Parse "SYSROOT <size>"
        if (header.substr(0, 8) != "SYSROOT ") {
            std::cerr << "unexpected response: " << header;
            close(fd);
            return 1;
        }
        size_t size = std::stoull(header.substr(8));
        std::cerr << "downloading sysroot: " << size << " bytes..." << std::endl;

        // Save to temp file then extract
        std::string tmppath = dest_dir + "/.sysroot-download.tar";
        std::ofstream out(tmppath, std::ios::binary);
        if (!out) {
            std::cerr << "failed to create " << tmppath << "\n";
            close(fd);
            return 1;
        }

        // read_all may have consumed binary data past the header newline
        size_t remaining = size;
        size_t total_read = 0;
        auto nl = header.find('\n');
        if (nl != std::string::npos && nl + 1 < header.size()) {
            std::string overflow = header.substr(nl + 1);
            size_t take = std::min(overflow.size(), remaining);
            out.write(overflow.data(), static_cast<std::streamsize>(take));
            remaining -= take;
            total_read += take;
        }

        char buf[65536];
        while (remaining > 0) {
            size_t chunk = std::min(remaining, sizeof(buf));
            ssize_t n = read(fd, buf, chunk);
            if (n <= 0) {
                std::cerr << "download interrupted at " << total_read << "/" << size << "\n";
                break;
            }
            out.write(buf, n);
            remaining -= static_cast<size_t>(n);
            total_read += static_cast<size_t>(n);

            // Print progress every ~1MB
            if (total_read % (1024 * 1024) < static_cast<size_t>(n)) {
                std::cerr << "\r  " << (total_read / (1024 * 1024)) << " / "
                          << (size / (1024 * 1024)) << " MB" << std::flush;
            }
        }
        out.close();
        close(fd);
        std::cerr << "\r  " << (size / (1024 * 1024)) << " / "
                  << (size / (1024 * 1024)) << " MB - done" << std::endl;

        if (total_read != size) {
            std::cerr << "incomplete download\n";
            unlink(tmppath.c_str());
            return 1;
        }

        // Extract
        std::cerr << "extracting to " << dest_dir << "..." << std::endl;
        std::string tar_cmd = "tar xf " + tmppath + " -C " + dest_dir;
        int ret = system(tar_cmd.c_str());
        unlink(tmppath.c_str());

        if (ret != 0) {
            std::cerr << "extraction failed\n";
            return 1;
        }

        std::cerr << "sysroot saved to " << dest_dir << std::endl;
        std::cout << "{\"ok\":true,\"path\":\"" << dest_dir << "\"}\n";
        return 0;
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
