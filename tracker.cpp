#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

const int DEFAULT_PORT = 7000;
const int DEFAULT_TIMEOUT = 15; // segundos sem noticia ate dar o peer como caido
const size_t LINE_MAX_LEN = 1024;

std::mutex log_mutex;

// uma linha de log com data e categoria (CONN, OP, ERRO, INFO)
void log_event(const char *category, const std::string &msg) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    std::lock_guard<std::mutex> lock(log_mutex);
    printf("[%s] [%s] %s\n", timebuf, category, msg.c_str());
    fflush(stdout);
}

// o que o tracker sabe de cada peer
struct Peer {
    std::string ip;                 // vem da conexao
    int port;                       // porta onde o peer atende outros peers
    std::set<std::string> contents; // nomes dos conteudos que ele tem
};

class Tracker {
public:
    // executa um comando e devolve a resposta; quit vira true no QUIT
    std::string handle(int fd, const std::string &ip, const std::string &line, bool &quit);

    // tira o peer do registro quando a conexao fecha
    void remove_peer(int fd);

    bool is_registered(int fd);

private:
    std::map<int, Peer> peers; // a chave e o fd da conexao do peer
    std::mutex mtx;
    long commands = 0;         // comandos atendidos, fora o PING
    time_t started = time(NULL);
};

std::vector<std::string> split(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream in(line);
    std::string tok;
    while (in >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

std::string Tracker::handle(int fd, const std::string &ip, const std::string &line, bool &quit) {
    std::vector<std::string> t = split(line);
    if (t.empty()) {
        return "ERR EMPTY_COMMAND linha vazia";
    }
    std::string cmd = t[0];
    size_t n_args = t.size() - 1;

    if (cmd == "QUIT") {
        quit = true;
        return "OK bye";
    }
    if (cmd == "PING") {
        return "OK pong";
    }

    std::lock_guard<std::mutex> lock(mtx);
    commands++;

    // monitoramento: um retrato do estado do tracker
    if (cmd == "STATS") {
        std::set<std::string> all;
        for (auto &p : peers) {
            all.insert(p.second.contents.begin(), p.second.contents.end());
        }
        return "OK peers=" + std::to_string(peers.size()) +
               " conteudos=" + std::to_string(all.size()) +
               " comandos=" + std::to_string(commands) +
               " ligado_ha=" + std::to_string(time(NULL) - started) + "s";
    }

    if (cmd == "REGISTER") {
        if (n_args != 1) {
            return "ERR BAD_ARGS uso: REGISTER <porta>";
        }
        int port = atoi(t[1].c_str());
        if (port <= 0 || port > 65535) {
            return "ERR BAD_ARGS porta invalida: " + t[1];
        }
        // registrar de novo so troca a porta, os conteudos continuam
        peers[fd].ip = ip;
        peers[fd].port = port;
        return "OK registered " + ip + ":" + std::to_string(port);
    }

    if (cmd == "ANNOUNCE") {
        if (n_args != 1) {
            return "ERR BAD_ARGS uso: ANNOUNCE <conteudo>";
        }
        if (peers.count(fd) == 0) {
            return "ERR NOT_REGISTERED mande REGISTER antes";
        }
        peers[fd].contents.insert(t[1]);
        return "OK announced " + t[1];
    }

    if (cmd == "LIST") {
        if (n_args != 0) {
            return "ERR BAD_ARGS uso: LIST";
        }
        std::set<std::string> all;
        for (auto &p : peers) {
            all.insert(p.second.contents.begin(), p.second.contents.end());
        }
        std::string resp = "OK";
        for (auto &name : all) {
            resp += " " + name;
        }
        return resp;
    }

    if (cmd == "PEERS") {
        if (n_args != 1) {
            return "ERR BAD_ARGS uso: PEERS <conteudo>";
        }
        std::string resp = "OK";
        int found = 0;
        for (auto &p : peers) {
            if (p.second.contents.count(t[1]) > 0) {
                resp += " " + p.second.ip + ":" + std::to_string(p.second.port);
                found++;
            }
        }
        if (found == 0) {
            return "ERR NOT_FOUND nenhum peer tem: " + t[1];
        }
        return resp;
    }

    return "ERR UNKNOWN_COMMAND comando desconhecido: " + cmd;
}

void Tracker::remove_peer(int fd) {
    std::lock_guard<std::mutex> lock(mtx);
    peers.erase(fd);
}

bool Tracker::is_registered(int fd) {
    std::lock_guard<std::mutex> lock(mtx);
    return peers.count(fd) > 0;
}

// endereco de quem conectou, em texto. Cliente IPv4 chega no soquete IPv6
// como ::ffff:a.b.c.d e e escrito so como a.b.c.d; endereco IPv6 vai entre
// colchetes, pra nao confundir os ':' dele com o da porta.
std::string addr_text(const struct in6_addr &addr) {
    char text[INET6_ADDRSTRLEN];
    if (IN6_IS_ADDR_V4MAPPED(&addr)) {
        inet_ntop(AF_INET, &addr.s6_addr[12], text, sizeof(text));
        return text;
    }
    inet_ntop(AF_INET6, &addr, text, sizeof(text));
    return "[" + std::string(text) + "]";
}

bool send_line(int fd, const std::string &msg) {
    std::string out = msg + "\n";
    return write(fd, out.c_str(), out.size()) == (ssize_t)out.size();
}

// atende um peer ate ele desconectar
void serve_peer(Tracker &tracker, int fd, std::string ip, int port, int timeout) {
    std::string who = ip + ":" + std::to_string(port);
    std::string line;
    char chunk[512];
    bool quit = false;

    // o read desiste depois de 'timeout' segundos sem chegar nada
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!quit) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
            // quem so consulta (nao registrou) pode ficar parado
            if (!tracker.is_registered(fd)) {
                continue;
            }
            log_event("CONN", "peer sem sinal de vida ha " + std::to_string(timeout) + "s: " + who);
            break;
        }
        for (ssize_t i = 0; i < n && !quit; i++) {
            char c = chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c != '\n') {
                if (line.size() < LINE_MAX_LEN) {
                    line += c;
                }
                continue;
            }
            std::string resp = tracker.handle(fd, ip, line, quit);
            // PING nao vai pro log, senao ele so teria PING
            if (line != "PING") {
                const char *category = (resp.compare(0, 3, "ERR") == 0) ? "ERRO" : "OP";
                log_event(category, who + " '" + line + "' -> " + resp);
            }
            line.clear();
            if (!send_line(fd, resp)) {
                quit = true;
            }
        }
    }

    // sem isso o peer que caiu continuaria aparecendo no PEERS
    tracker.remove_peer(fd);
    log_event("CONN", "peer desconectado: " + who);
    close(fd);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int timeout = DEFAULT_TIMEOUT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (argc > 2) {
        timeout = atoi(argv[2]);
    }

    // escrever em conexao ja fechada da erro no write em vez de matar o processo
    signal(SIGPIPE, SIG_IGN);

    // um soquete IPv6 com V6ONLY desligado atende IPv6 e IPv4 na mesma porta
    int server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1, off = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }
    log_event("INFO", "tracker escutando na porta " + std::to_string(port) +
                      ", tempo limite " + std::to_string(timeout) + "s");

    Tracker tracker;
    while (true) {
        struct sockaddr_in6 peer_addr;
        socklen_t len = sizeof(peer_addr);
        int fd = accept(server_fd, (struct sockaddr *)&peer_addr, &len);
        if (fd < 0) {
            perror("accept");
            continue;
        }
        std::string ip = addr_text(peer_addr.sin6_addr);
        int peer_port = ntohs(peer_addr.sin6_port);
        log_event("CONN", "peer conectado: " + ip + ":" + std::to_string(peer_port));

        // uma thread por conexao
        std::thread(serve_peer, std::ref(tracker), fd, ip, peer_port, timeout).detach();
    }
}
