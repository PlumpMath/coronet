#include "coronet.hpp"

int main() {
    coronet coro;
    coro.listen("0.0.0.0", 1234, [&](int fd) {
        while (true) {
            auto vec = coro.read(fd);
            if (vec.size() == 0) break;
            coro.write(fd, vec);
        }
    });
    coro.run();
}
