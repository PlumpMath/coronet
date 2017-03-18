#include <coronet.hpp>

int main() {
    coronet coro;
    coro.listen("0.0.0.0", 8080, [&](int fd) {
        auto& out = coro.get_ostream(fd);
        out << "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-type: text/plain\r\nContent-length: 10\r\n\r\nIt works!\n";
        coro.close(fd);
    });
    coro.run();
}
