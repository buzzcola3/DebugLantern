#ifndef DEBUGLANTERN_WEBUI_H
#define DEBUGLANTERN_WEBUI_H

#include <atomic>
#include <string>
#include <thread>

namespace debuglantern {

class WebUI {
public:
    WebUI(int web_port, int control_port);
    ~WebUI();

    bool start();
    void stop();

private:
    void run();
    void handle_client(int fd);
    void serve_sse(int fd);

    std::string proxy(const std::string &command);
    std::string proxy_upload(const char *data, size_t len);

    int web_port_;
    int control_port_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace debuglantern

#endif  // DEBUGLANTERN_WEBUI_H
