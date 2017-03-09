#include <boost/context/all.hpp>
#include <vector>
#include <functional>
#include <unistd.h>
#include <iostream>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <system_error>
#include <stdexcept>
#include <string>
#include <string.h>

class coronet {
    static const size_t block_size = 1024;
    static const size_t max_events = 1024;
    // The sub-contexts send true if they are waiting for a write,
    // false otherwise.
    boost::context::execution_context<bool> main_context;
    std::vector<boost::context::execution_context<bool>> running_contextes;
    std::vector<std::function<void(int)>> connection_handlers;
    std::vector<bool> is_listening;
    int epoll;

    template<typename T, typename U> 
    void set_vector(std::vector<T>& vec, int pos, U&& val) {
        if (pos < 0) throw std::out_of_range("Negative position");
        if (vec.size() <= (unsigned long long) pos) vec.resize(pos+1);
        vec[pos] = std::move(val);
    }

    void add_watched_socket(int fd) {
        struct epoll_event watched;
        watched.events = EPOLLIN;
        watched.data.fd = fd;
        if (epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &watched) == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }

    void edit_watched_socket(int fd, bool write = false) {
        struct epoll_event watched;
        watched.events = write ? EPOLLOUT : EPOLLIN;
        watched.data.fd = fd;
        if (epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &watched) == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }

    void resume_subcontext(int fd) {
        auto tmp = running_contextes[fd](true);
        running_contextes[fd] = std::move(std::get<0>(tmp));
        if (running_contextes[fd]) {
            edit_watched_socket(fd, std::get<1>(tmp));
        }
    }

    void do_accept(int fd, int listen_fd) {
        add_watched_socket(fd);
        if (!connection_handlers[listen_fd]) {
            throw std::runtime_error("invalid client function");
        }
        auto childfun = [=] (boost::context::execution_context<bool>&& ctx, bool val) mutable {
            try {
                main_context = std::move(ctx);
                connection_handlers[listen_fd](fd);
            } catch (std::exception& exc) {
                std::cerr << "Exception in subcontext: " << exc.what() << std::endl;
            }
            close(fd);
            return std::move(main_context);
        };
        boost::context::execution_context<bool> subctx(childfun);
        set_vector(running_contextes, fd, std::move(subctx));
        resume_subcontext(fd);
    }

    void handle_ready_socket(int fd, uint32_t event) {
        try {
            if (event & EPOLLERR || event & EPOLLHUP) {
                close(fd);
                throw std::runtime_error("Something went wrong with fd " + std::to_string(fd));
            }
            if (event & EPOLLIN && is_listening.size() > (unsigned) fd && is_listening[fd]) {
                int client = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK);
                if (client == -1) {
                    throw std::system_error(errno, std::generic_category(), "accept4");
                }
                do_accept(client, fd);
                return;
            }
            if (is_listening.size() > (unsigned) fd && is_listening[fd]) {
                throw std::runtime_error("Invalid event");
            }
            resume_subcontext(fd);
        } catch (std::exception& exc) {
            std::cerr << "Exception in main context " << exc.what() << std::endl;
        }
    }

public:
    coronet() {
        epoll = epoll_create(1);
    }

    std::size_t read(int fd, uint8_t* data, std::size_t data_size) {
        while (true) {
            ssize_t read_now = ::read(fd, data, data_size);
            if (read_now == -1 && errno != EAGAIN) {
                throw std::system_error(errno, std::generic_category(), "read");
            }
            if (read_now == -1 && errno == EAGAIN) {
                // Do context switching and try again
                auto tmp = main_context(false);
                main_context = std::move(std::get<0>(tmp));
                continue;
            }
            return read_now;
        }
    }

    std::size_t read(int fd, std::vector<uint8_t>& data) {
        return read(fd, data.data(), data.size());
    }

    std::vector<uint8_t> read(int fd, std::size_t size = block_size) {
        std::vector<uint8_t> res(size);
        res.resize(read(fd, res));
        return res;
    }

    std::size_t read_all(int fd, uint8_t* data, std::size_t data_size) {
        std::size_t read_so_far = 0;
        std::size_t current_read = 0;
        do {
            current_read = read(fd, data+read_so_far, data_size-read_so_far);
            read_so_far += current_read;
        } while (current_read != 0);
        return read_so_far;
    }

    std::size_t read_all(int fd, std::vector<uint8_t>& data) {
        return read_all(fd, data.data(), data.size());
    }

    std::vector<uint8_t> read_all(int fd) {
        std::vector<uint8_t> res(block_size);
        std::size_t read_so_far = 0;
        std::size_t current_read = 0;
        do {
            if (read_so_far + block_size/2 > res.size()) {
                res.resize(res.size() + block_size);
            }
            current_read = read(fd, res.data()+read_so_far, res.size()-read_so_far);
            read_so_far += current_read;
        } while (current_read != 0);
        res.resize(read_so_far);
        return res;
    }

    void write(int fd, const uint8_t* data, std::size_t data_size) {
        std::size_t written_so_far = 0;
        do {
            ssize_t written_now = ::write(fd, data+written_so_far, data_size-written_so_far);
            if (written_now == -1 && errno != EAGAIN) {
                throw std::system_error(errno, std::generic_category(), "write");
            }
            if (written_now == -1 && errno == EAGAIN) {
                // Do context switching
                auto tmp = main_context(true);
                main_context = std::move(std::get<0>(tmp));
            }
            written_so_far += written_now;
        } while (written_so_far < data_size);
    }

    void write(int fd, const std::vector<uint8_t>& data) {
        return write(fd, data.data(), data.size());
    }

    void listen(const std::string& s_addr, int port, std::function<void(int)>&& callback) {
        struct sockaddr_in server_address;
        memset(&server_address, 0, sizeof(server_address));
        if (inet_aton(s_addr.c_str(), &server_address.sin_addr) == 0) {
            throw std::runtime_error("Invalid address");
        }
        port = htons(port);
        server_address.sin_family = AF_INET;
        server_address.sin_port = port;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) {
            throw std::system_error(errno, std::generic_category(), "socket");
        }
        int one = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0) {
            throw std::system_error(errno, std::generic_category(), "setsockopt");
        }
        if (bind(sock, (struct sockaddr*)&server_address, sizeof(server_address))) {
            throw std::system_error(errno, std::generic_category(), "bind");
        }
        if (::listen(sock, SOMAXCONN) == -1) {
            throw std::system_error(errno, std::generic_category(), "listen");
        }
        if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
            throw std::system_error(errno, std::generic_category(), "fcntl");
        }
        add_watched_socket(sock);
        set_vector(connection_handlers, sock, callback);
        set_vector(is_listening, sock, true);
    }

    void run() {
        while (true) {
            std::vector<struct epoll_event> events(max_events);
            int num_fd = epoll_wait(epoll, events.data(), max_events, -1);
            if (num_fd == -1) {
                throw std::system_error(errno, std::generic_category(), "epoll_wait");
            }
            for (int i=0; i<num_fd; i++) {
                handle_ready_socket(events[i].data.fd, events[i].events);
            }
        }
    }
};
