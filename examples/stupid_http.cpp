#include <coronet.hpp>

int main() {
    coronet coro;
    coro.listen("0.0.0.0", 8080, [&](int fd) {
        const char resp[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-type: text/plain\r\nContent-length: 10\r\n\r\nIt works!\n";
        coro.write(fd, (const uint8_t*) resp, sizeof(resp)-1);
    });
    coro.run();
}
