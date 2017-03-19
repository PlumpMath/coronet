#include <coronet.hpp>

int main() {
    coronet coro;
    coro.listen("0.0.0.0", 8080, [&](int fd) {
        auto& out = coro.get_ostream(fd);
        auto& in = coro.get_istream(fd);
        std::string header_line;
        bool keep_alive = true;
        while (true) {
            while (std::getline(in, header_line)) {
                if (header_line.size() == 0) break;
                if (header_line.back() == '\r') header_line.pop_back();
                if (header_line.size() == 0) break;
                if (header_line.find("HTTP/1.0") != std::string::npos) keep_alive = false;
                if (header_line.find("Connection: close") != std::string::npos) keep_alive = false;
                if (header_line.find("Connection: Keep-Alive") != std::string::npos) keep_alive = true;
            }
            if (!in) break;
            out << "HTTP/1.1 200 OK\r\n";
            if (!keep_alive) out << "Connection: close\r\n";
            else out << "Connection: keep-alive\r\n";
            out << "Content-type: text/plain\r\nContent-length: 10\r\n\r\nIt works!\n" << std::flush;
            if (!keep_alive) break;
        }
        coro.close(fd);
    });
    coro.run();
}
