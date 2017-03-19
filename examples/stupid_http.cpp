#include <coronet.hpp>

int main() {
    coronet coro;
    coro.listen("0.0.0.0", 8080, [&](int fd) {
        auto& out = coro.get_ostream(fd);
        auto& in = coro.get_istream(fd);
        std::string header_line;
        while (std::getline(in, header_line) && header_line != "\r") {}
        out << "HTTP/1.1 200 OK\r\nContent-type: text/plain\r\nContent-length: 10\r\n\r\nIt works!\n";
        coro.close(fd);
    });
    coro.run();
}
