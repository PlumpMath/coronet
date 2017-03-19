#ifndef _CORONET_HPP
#define _CORONET_HPP
#include <boost/context/all.hpp>
#include <vector>
#include <functional>
#include <iostream>
#include <system_error>
#include <stdexcept>
#include <string>
#include <streambuf>
#include <chrono>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <string.h>

static const size_t block_size = 1024;

class coronet;

class coroibuf: public std::basic_streambuf<char> {
    coronet& coro_;
    int fd_;
    const std::size_t put_back_;
    std::vector<char> buffer_;
public:
    explicit coroibuf(
        int fd, coronet& coro,
        std::size_t buff_sz = block_size, std::size_t put_back = 8
    ): coro_(coro), fd_(fd), put_back_(std::max(put_back, size_t(1))),
       buffer_(std::max(buff_sz, put_back_) + put_back_) {
        char* end = &buffer_.front() + buffer_.size();
        setg(end, end, end);
    }

    coroibuf(const coroibuf &) = delete;
    coroibuf& operator=(const coroibuf &) = delete;

private:
    int_type underflow();
};

class coroobuf: public std::basic_streambuf<char> {
    coronet& coro_;
    int fd_;
    std::vector<char> buffer_;
public:
    explicit coroobuf(
        int fd, coronet& coro,
        std::size_t buff_sz = block_size
    ): coro_(coro), fd_(fd), buffer_(buff_sz + 1) {
        char* base = &buffer_.front();
        setp(base, base + buffer_.size() - 1); // -1 to make overflow() easier
    }

    coroobuf(const coroobuf &) = delete;
    coroobuf& operator=(const coroobuf &) = delete;

private:
    int_type overflow(int_type ch);
    int sync();
};

class coronet {
    static const size_t max_events = 1024;
    // The sub-contexts send true if they are waiting for a write,
    // false otherwise.
    boost::context::execution_context<bool> main_context;
    std::vector<boost::context::execution_context<bool>> running_contextes;
    std::vector<std::function<void(int)>> connection_handlers;
    std::vector<bool> is_listening;
    std::vector<bool> is_closed;
    std::vector<bool> is_good_;
    std::vector<bool> has_timed_out;
    std::vector<int> timer_fds;
    std::vector<int> timer_for;
    std::vector<std::unique_ptr<coroibuf>> ibufs;
    std::vector<std::unique_ptr<std::istream>> istreams;
    std::vector<std::unique_ptr<coroobuf>> obufs;
    std::vector<std::unique_ptr<std::ostream>> ostreams;
    int running_ctxes = 0;
    int epoll;

    template<typename T, typename U>
    void set_vector(std::vector<T>& vec, int pos, U&& val) {
        if (pos < 0) throw std::out_of_range("Negative position");
        if (vec.size() <= (unsigned long long) pos) vec.resize(pos+1);
        vec[pos] = std::move(val);
    }

    template<typename T, typename U>
    void set_vector(std::vector<T>& vec, int pos, U&& val, T def) {
        if (pos < 0) throw std::out_of_range("Negative position");
        if (vec.size() <= (unsigned long long) pos) vec.resize(pos+1, def);
        vec[pos] = std::move(val);
    }

    void add_watched_socket(int fd) {
        struct epoll_event watched;
        watched.events = (EPOLLIN | EPOLLRDHUP);
        watched.data.fd = fd;
        if (epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &watched) == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }

    void edit_watched_socket(int fd, bool write = false) {
        struct epoll_event watched;
        watched.events = EPOLLRDHUP | (write ? EPOLLOUT : EPOLLIN);
        watched.data.fd = fd;
        if (epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &watched) == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }

    void remove_watched_socket(int fd) {
        if (epoll_ctl(epoll, EPOLL_CTL_DEL, fd, NULL) == -1) {
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
            running_ctxes++;
            try {
                main_context = std::move(ctx);
                connection_handlers[listen_fd](fd);
            } catch (timeout& t) {
                std::cerr << t.what() << std::endl;
            } catch (std::exception& exc) {
                std::cerr << "Exception in subcontext: " << exc.what() << std::endl;
            }
            running_ctxes--;
            close(fd, true);
            return std::move(main_context);
        };

        int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
        set_vector(timer_fds, fd, tfd);
        set_vector(timer_for, tfd, fd, -1);
        set_vector(has_timed_out, fd, false);
        using namespace std::chrono_literals;
        set_timeout(fd, 10s);
        add_watched_socket(tfd);

        set_vector(ibufs, fd, std::make_unique<coroibuf>(fd, *this));
        auto ist = std::make_unique<std::istream>(ibufs[fd].get());
        ist->exceptions(std::istream::badbit);
        set_vector(istreams, fd, ist);
        set_vector(obufs, fd, std::make_unique<coroobuf>(fd, *this));
        auto ost = std::make_unique<std::ostream>(obufs[fd].get());
        ost->exceptions(std::ostream::badbit);
        set_vector(ostreams, fd, ost);
        set_vector(is_closed, fd, false);
        set_vector(is_good_, fd, true);

        boost::context::execution_context<bool> subctx{childfun};
        set_vector(running_contextes, fd, std::move(subctx));
        resume_subcontext(fd);
    }

    void handle_ready_socket(int fd, uint32_t event) {
        try {
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
            if (event & EPOLLIN && timer_for.size() > (unsigned) fd && timer_for[fd] != -1) {
                fd = timer_for[fd];
                has_timed_out[fd] = true;
            }
            resume_subcontext(fd);
        } catch (std::exception& exc) {
            std::cerr << "Exception in main context: " << exc.what() << std::endl;
        }
    }

public:
    class timeout: public std::runtime_error {
    public:
        timeout(const std::string& msg): std::runtime_error(msg) {}
    };

    coronet() {
        epoll = epoll_create(1);
    }

    std::size_t read(int fd, char* data, std::size_t data_size) {
        while (true) {
            ssize_t read_now = ::read(fd, data, data_size);
            if (read_now == -1 && errno != EAGAIN) {
                is_good_[fd] = false;
                throw std::system_error(errno, std::generic_category(), "read " + std::to_string(fd));
            }
            if (read_now == -1 && errno == EAGAIN) {
                // Do context switching and try again
                auto tmp = main_context(false);
                main_context = std::move(std::get<0>(tmp));
                // If a timeout happened...
                if (has_timed_out.size() > (unsigned) fd && has_timed_out[fd]) {
                    throw timeout("read timeout: " + std::to_string(fd));
                }
                continue;
            }
            return read_now;
        }
    }

    void write(int fd, const char* data, std::size_t data_size) {
        std::size_t written_so_far = 0;
        do {
            ssize_t written_now = ::send(fd, data+written_so_far, data_size-written_so_far, MSG_NOSIGNAL);
            if (written_now == -1 && errno != EAGAIN) {
                is_good_[fd] = false;
                throw std::system_error(errno, std::generic_category(), "write " + std::to_string(fd));
            }
            if (written_now == -1 && errno == EAGAIN) {
                // Do context switching
                auto tmp = main_context(true);
                main_context = std::move(std::get<0>(tmp));
                // If a timeout happened...
                if (has_timed_out.size() > (unsigned) fd && has_timed_out[fd]) {
                    throw timeout("write timeout: " + std::to_string(fd));
                }
            }
            written_so_far += written_now;
        } while (written_so_far < data_size);
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
        std::vector<struct epoll_event> events(max_events);
        while (true) {
            int num_fd = epoll_wait(epoll, events.data(), max_events, -1);
            if (num_fd == -1 && errno == EINTR) continue;
            if (num_fd == -1) {
                throw std::system_error(errno, std::generic_category(), "epoll_wait");
            }
            for (int i=0; i<num_fd; i++) {
                handle_ready_socket(events[i].data.fd, events[i].events);
            }
        }
    }

    void close(int fd, bool nosync=false) {
        if (is_closed[fd]) return;
        timer_for[timer_fds[fd]] = -1;
        remove_watched_socket(timer_fds[fd]);
        ::close(timer_fds[fd]);
        is_closed[fd] = true;
        if (!nosync) *ostreams[fd] << std::flush;
        remove_watched_socket(fd);
        ::close(fd);
    }

    /**
     *  Get input/output streams. If two different coroutines use the same
     * i/ostream the result is undefined. The streams are guaranteed to be
     * valid as long as the corresponding file descriptor is not closed.
     */
    std::istream& get_istream(int fd) {return *istreams[fd];}
    std::ostream& get_ostream(int fd) {return *ostreams[fd];}

    bool is_good(int fd) {return is_good_[fd];}

    void set_timeout(int fd, std::chrono::duration<long int> to) {
        using namespace std::chrono;
        struct itimerspec ts;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 0;
        ts.it_value.tv_sec = duration_cast<seconds>(to).count();
        ts.it_value.tv_nsec = duration_cast<nanoseconds>(to).count() % 1000000000;
        timerfd_settime(timer_fds[fd], 0, &ts, nullptr);
    }
};

coroibuf::int_type coroibuf::underflow() {
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

    char *base = &buffer_.front();
    char *start = base;
    if (eback() == base) { // true when this isn't the first fill
        // Make arrangements for putback characters
        memmove(base, egptr() - put_back_, put_back_);
        start += put_back_;
    }
    // start is now the start of the buffer, proper.
    // Read from fptr_ in to the provided buffer
    size_t n = 0;
    if (!coro_.is_good(fd_)) return traits_type::eof();
    n = coro_.read(fd_, start, buffer_.size() - (start - base));
    if (n == 0) return traits_type::eof();
    // Set buffer pointers
    setg(base, start, start + n);
    return traits_type::to_int_type(*gptr());
}

coroobuf::int_type coroobuf::overflow(coroobuf::int_type ch) {
    if (!coro_.is_good(fd_) || ch == traits_type::eof()) return traits_type::eof();
    *pptr() = ch;
    pbump(1);
    if (sync() == -1) return traits_type::eof();
    return ch;
}

int coroobuf::sync() {
    if (!coro_.is_good(fd_)) return -1;
    std::ptrdiff_t n = pptr() - pbase();
    pbump(-n);
    coro_.write(fd_, pbase(), n);
    return 0;
}
#endif
