
#ifndef CONN_H
#define CONN_H

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <string>

class Conn {
public:
    Conn() {}
    explicit Conn(int fd) : fd(fd) {}

    bool connect_to(const std::string &host, const std::string &port) {
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC; // IPv4 ou IPv6, o que o nome tiver
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
            return false;
        }
        // um nome pode ter varios enderecos: fica com o primeiro que conectar
        bool ok = false;
        for (struct addrinfo *a = res; a != NULL && !ok; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            ok = fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0;
            if (!ok) {
                close_conn();
            }
        }
        freeaddrinfo(res);
        return ok;
    }

    // o write pode mandar so um pedaco, entao repete ate ir tudo
    bool send_bytes(const char *data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = write(fd, data + sent, len - sent);
            if (n <= 0) {
                return false;
            }
            sent += n;
        }
        return true;
    }

    bool send_line(const std::string &line) {
        std::string out = line + "\n";
        return send_bytes(out.c_str(), out.size());
    }

    // le ate o proximo '\n'; devolve false se a conexao fechou
    bool read_line(std::string &line) {
        while (true) {
            size_t pos = pending.find('\n');
            if (pos != std::string::npos) {
                line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return true;
            }
            if (!fill()) {
                return false;
            }
        }
    }

    // le exatamente n bytes
    bool read_bytes(std::string &out, size_t n) {
        while (pending.size() < n) {
            if (!fill()) {
                return false;
            }
        }
        out = pending.substr(0, n);
        pending.erase(0, n);
        return true;
    }

    // tempo limite de leitura: se nao chegar nada em 'seconds' segundos, a
    // leitura falha como se a conexao tivesse caido
    void set_timeout(int seconds) {
        struct timeval tv;
        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    void close_conn() {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

private:
    bool fill() {
        char chunk[4096];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            return false;
        }
        pending.append(chunk, n);
        return true;
    }

    int fd = -1;
    std::string pending; // bytes que ja chegaram e ainda nao foram usados
};

#endif
