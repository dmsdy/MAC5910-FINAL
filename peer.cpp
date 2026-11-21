#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "conn.h"

const size_t PART_SIZE = 64 * 1024;
const int DEFAULT_PING_INTERVAL = 5; // segundos entre um PING e outro
const int MAX_TRIES = 3;             // quantas vezes pede a mesma parte
const int SOURCE_TIMEOUT = 5;        // segundos sem resposta ate desistir de uma fonte

// o que vem da linha de comando
struct Options {
    int interval = DEFAULT_PING_INTERVAL;
    double corrupt = 0.0; // chance de estragar uma parte enviada (so pra teste)
    int delay_ms = 0;     // espera antes de enviar cada parte (so pra teste)
    double rate = 1.0;    // partes por segundo que o player assiste
    size_t buffer = 3;    // partes que o player junta antes de tocar
    size_t window = 10;   // no play, quantas partes o download pode ficar a frente do player
    std::string key;      // chave compartilhada do grupo
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

// SHA-256 dos dados, escrito em hexadecimal (64 caracteres)
std::string sha256_hex(const std::string &data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data.data(), data.size(), digest);
    char hex[2 * SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    return std::string(hex);
}

// HMAC-SHA256 dos dados com a chave, em hexadecimal: o carimbo que so quem
// tem a chave consegue calcular
std::string hmac_hex(const std::string &key, const std::string &data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), key.size(), (const unsigned char *)data.data(), data.size(),
         digest, &len);
    char hex[2 * SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    return std::string(hex);
}

// o texto que e carimbado: nome, tamanho e todos os hashes do conteudo
std::string info_text(const std::string &name, size_t size, const std::vector<std::string> &hashes) {
    std::string text = name + "\n" + std::to_string(size) + "\n";
    for (auto &h : hashes) {
        text += h + "\n";
    }
    return text;
}

// um conteudo que este peer tem completo no disco
struct Content {
    std::string path;
    size_t size = 0;
    std::vector<std::string> hashes; // um SHA-256 por parte
};

// os conteudos que este peer pode servir
class Library {
public:
    // le o arquivo e calcula o hash de cada parte
    bool add(const std::string &name, const std::string &path);
    bool get(const std::string &name, Content &out);
    std::vector<std::string> names();

private:
    std::map<std::string, Content> contents;
    std::mutex mtx;
};

bool Library::add(const std::string &name, const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    Content c;
    c.path = path;
    std::string part(PART_SIZE, '\0');
    while (in.read(&part[0], PART_SIZE) || in.gcount() > 0) {
        size_t n = in.gcount();
        c.hashes.push_back(sha256_hex(part.substr(0, n)));
        c.size += n;
    }
    std::lock_guard<std::mutex> lock(mtx);
    contents[name] = c;
    return true;
}

bool Library::get(const std::string &name, Content &out) {
    std::lock_guard<std::mutex> lock(mtx);
    if (contents.count(name) == 0) {
        return false;
    }
    out = contents[name];
    return true;
}

std::vector<std::string> Library::names() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> out;
    for (auto &c : contents) {
        out.push_back(c.first);
    }
    return out;
}

// Buffer de recepcao: a ponte entre quem baixa e o player. Guarda quais
// partes ja chegaram (os bytes ficam no disco). Como elas chegam fora de
// ordem, o player so pode contar com as que formam um trecho sem buraco
// desde o comeco.
class StreamBuffer {
public:
    // quem baixa avisa quantas partes o conteudo tem
    void start(size_t n_parts, size_t already = 0) {
        std::lock_guard<std::mutex> lock(mtx);
        total = n_parts;
        have.assign(n_parts, false);
        ready = already;
        started = true;
        cv.notify_all();
    }

    void part_arrived(size_t index) {
        std::lock_guard<std::mutex> lock(mtx);
        have[index] = true;
        while (ready < total && have[ready]) {
            ready++;
        }
        cv.notify_all();
    }

    void fail() {
        std::lock_guard<std::mutex> lock(mtx);
        failed = true;
        cv.notify_all();
    }

    // o player avisa quantas partes ja tocou
    void set_played(size_t n) {
        std::lock_guard<std::mutex> lock(mtx);
        played = n;
        cv.notify_all();
    }

    // quem baixa espera aqui ate a parte caber na janela, ou seja, ate ela
    // estar a menos de 'window' partes do que o player ja tocou. Espera no
    // maximo um pouquinho e devolve se coube: quem chamou pode ter recebido
    // nesse meio tempo uma parte mais urgente, de uma fonte que caiu.
    bool wait_room(size_t index, size_t window) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(100),
                    [&] { return failed || index < played + window; });
        return index < played + window;
    }

    // espera ate ter n partes (ou todas, se o conteudo tem menos que n);
    // devolve false se o download falhou antes disso
    bool wait_for(size_t n) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return failed || (started && ready >= std::min(n, total)); });
        return started && ready >= std::min(n, total);
    }

    size_t parts_ready() {
        std::lock_guard<std::mutex> lock(mtx);
        return ready;
    }

    size_t parts_total() {
        std::lock_guard<std::mutex> lock(mtx);
        return total;
    }

private:
    std::mutex mtx;
    std::condition_variable cv;
    bool started = false; // ja sabe o total de partes
    bool failed = false;
    size_t total = 0;
    std::vector<bool> have; // quais partes ja chegaram e conferem
    size_t ready = 0;       // as partes 0..ready-1 ja chegaram todas
    size_t played = 0;      // quantas partes o player ja tocou
};

// conexao de controle com o tracker: manda uma linha, espera uma linha
class TrackerClient {
public:
    bool connect_to(const std::string &host, const std::string &port) {
        return conn.connect_to(host, port);
    }

    // devolve "" se a conexao caiu
    std::string request(const std::string &cmd) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string resp;
        if (!conn.send_line(cmd) || !conn.read_line(resp)) {
            return "";
        }
        return resp;
    }

    void close_conn() { conn.close_conn(); }

private:
    Conn conn;
    std::mutex mtx; // o teclado e o sinal de vida usam a mesma conexao
};

class Peer {
public:
    Peer(int port, const std::string &dir, const Options &opt)
        : port(port), dir(dir), opt(opt) {}

    bool start_listening();
    bool join(const std::string &host, const std::string &tracker_port);
    void run(); // le comandos do teclado ate quit

private:
    void load_dir();
    void heartbeat();
    void accept_loop();
    void serve(int fd);
    bool download(const std::string &name, StreamBuffer *buf = nullptr);
    bool fetch_info(Conn &conn, const std::string &source, const std::string &name,
                    size_t &size, std::vector<std::string> &hashes);
    bool fetch_part(Conn &conn, const std::string &name, size_t index,
                    const std::string &hash, std::string &part, std::atomic<int> &repeated);
    void play(const std::string &name);

    int port;        // porta onde este peer atende outros peers
    std::string dir; // pasta com os conteudos
    Options opt;
    int listen_fd = -1;
    Library library;
    TrackerClient tracker;

    // contadores do comando stats
    std::atomic<long> parts_sent{0}, bytes_sent{0};
    std::atomic<long> parts_received{0}, bytes_received{0};
    std::atomic<long> parts_bad{0};      // partes que chegaram erradas
    std::atomic<long> sources_lost{0};   // fontes que cairam durante um download
    std::atomic<long> lists_refused{0};  // listas de hashes sem carimbo valido
};

bool Peer::start_listening() {
    // um soquete IPv6 com V6ONLY desligado atende IPv6 e IPv4 na mesma porta
    listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return false;
    }
    int opt = 1, off = 0;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 10) < 0) {
        perror("bind/listen");
        return false;
    }
    return true;
}

// calcula os hashes de cada arquivo da pasta
void Peer::load_dir() {
    std::error_code ec;
    for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string name = entry.path().filename().string();
        if (name.find(' ') != std::string::npos) {
            std::cout << "pulando '" << name << "': nome com espaco\n";
            continue;
        }
        if (entry.path().extension() == ".part") {
            continue; // download que nao terminou
        }
        library.add(name, entry.path().string());
    }
    if (ec) {
        std::cout << "nao consegui ler a pasta " << dir << "\n";
    }
}

bool Peer::join(const std::string &host, const std::string &tracker_port) {
    load_dir();
    if (!tracker.connect_to(host, tracker_port)) {
        std::cerr << "nao consegui conectar no tracker " << host << ":" << tracker_port << "\n";
        return false;
    }
    std::string resp = tracker.request("REGISTER " + std::to_string(port));
    std::cout << "tracker: " << resp << "\n";
    if (resp.compare(0, 2, "OK") != 0) {
        return false;
    }
    for (auto &name : library.names()) {
        std::cout << "tracker: " << tracker.request("ANNOUNCE " + name) << "\n";
    }
    return true;
}

// roda numa thread separada: avisa o tracker que este peer continua vivo
void Peer::heartbeat() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(opt.interval));
        if (tracker.request("PING").empty()) {
            // o teclado esta parado esperando uma linha, entao sai direto
            // (_exit e nao exit: o exit ficaria preso esperando o teclado)
            std::cout << "perdi a conexao com o tracker" << std::endl;
            _exit(1);
        }
    }
}

// roda numa thread separada: aceita conexoes de outros peers
void Peer::accept_loop() {
    while (true) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        std::thread(&Peer::serve, this, fd).detach();
    }
}

// atende os pedidos de um peer ate ele fechar a conexao
void Peer::serve(int fd) {
    Conn conn(fd);
    std::string line;
    while (conn.read_line(line)) {
        std::vector<std::string> t = split(line);
        Content c;

        if (t.size() == 2 && t[0] == "INFO") {
            if (!library.get(t[1], c)) {
                conn.send_line("ERR NOT_FOUND nao tenho: " + t[1]);
                continue;
            }
            // tamanho, numero de partes e depois um hash por linha
            conn.send_line("OK " + std::to_string(c.size) + " " + std::to_string(c.hashes.size()));
            for (auto &h : c.hashes) {
                conn.send_line(h);
            }
            // por ultimo o carimbo da lista
            conn.send_line(hmac_hex(opt.key, info_text(t[1], c.size, c.hashes)));
            continue;
        }

        if (t.size() == 3 && t[0] == "GET") {
            if (!library.get(t[1], c)) {
                conn.send_line("ERR NOT_FOUND nao tenho: " + t[1]);
                continue;
            }
            size_t index = strtoul(t[2].c_str(), NULL, 10);
            if (index >= c.hashes.size()) {
                conn.send_line("ERR BAD_PART parte nao existe: " + t[2]);
                continue;
            }
            std::ifstream in(c.path, std::ios::binary);
            std::string part(PART_SIZE, '\0');
            in.seekg(index * PART_SIZE);
            in.read(&part[0], PART_SIZE);
            part.resize(in.gcount());

            // simulacao de erro: troca um byte da parte
            if (!part.empty() && (double)rand() / RAND_MAX < opt.corrupt) {
                part[rand() % part.size()] ^= 0xFF;
            }
            // simulacao de rede lenta: segura a parte um pouco antes de enviar
            if (opt.delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt.delay_ms));
            }

            conn.send_line("OK " + std::to_string(part.size()));
            if (conn.send_bytes(part.data(), part.size())) {
                parts_sent++;
                bytes_sent += part.size();
            }
            continue;
        }

        conn.send_line("ERR BAD_REQUEST pedido invalido");
    }
    conn.close_conn();
}

// pede a uma fonte o tamanho e a lista de hashes do conteudo e confere o
// carimbo; devolve false se a fonte nao respondeu direito ou se o carimbo
// nao bate
bool Peer::fetch_info(Conn &conn, const std::string &source, const std::string &name,
                      size_t &size, std::vector<std::string> &hashes) {
    std::string line, stamp;
    conn.send_line("INFO " + name);
    if (!conn.read_line(line)) {
        std::cout << source << " nao respondeu ao INFO\n";
        return false;
    }
    std::vector<std::string> t = split(line);
    if (t.size() != 3 || t[0] != "OK") {
        std::cout << source << ": " << line << "\n";
        return false;
    }
    size = strtoul(t[1].c_str(), NULL, 10);
    // o numero de partes sai do tamanho, nao do que a fonte diz
    hashes.assign((size + PART_SIZE - 1) / PART_SIZE, "");
    if (strtoul(t[2].c_str(), NULL, 10) != hashes.size()) {
        std::cout << source << " mandou um numero de partes que nao combina com o tamanho\n";
        return false;
    }
    for (auto &h : hashes) {
        if (!conn.read_line(h)) {
            std::cout << source << " nao respondeu ao INFO\n";
            return false;
        }
    }
    if (!conn.read_line(stamp)) {
        std::cout << source << " nao respondeu ao INFO\n";
        return false;
    }
    if (stamp != hmac_hex(opt.key, info_text(name, size, hashes))) {
        lists_refused++;
        std::cout << source << " mandou uma lista de hashes com carimbo invalido (chave diferente?)\n";
        return false;
    }
    return true;
}

// pede uma parte a uma fonte ate ela chegar certa (no maximo MAX_TRIES
// vezes); devolve false se desistiu ou se a conexao caiu
bool Peer::fetch_part(Conn &conn, const std::string &name, size_t index,
                      const std::string &hash, std::string &part, std::atomic<int> &repeated) {
    for (int attempt = 1; attempt <= MAX_TRIES; attempt++) {
        std::string line;
        conn.send_line("GET " + name + " " + std::to_string(index));
        if (!conn.read_line(line)) {
            return false;
        }
        std::vector<std::string> t = split(line);
        if (t.size() != 2 || t[0] != "OK") {
            return false;
        }
        size_t len = strtoul(t[1].c_str(), NULL, 10);
        if (len > PART_SIZE || !conn.read_bytes(part, len)) {
            return false;
        }
        if (sha256_hex(part) == hash) {
            parts_received++;
            bytes_received += part.size();
            return true;
        }
        parts_bad++;
        printf("parte %zu chegou errada (tentativa %d), pedindo de novo\n", index, attempt);
        repeated++;
    }
    return false;
}

// baixa um conteudo inteiro, de todos os peers que o tem ao mesmo tempo; se
// buf foi passado, avisa o player a cada parte que chega. Se uma fonte cai,
// as outras assumem as partes dela. Devolve false se falhou.
bool Peer::download(const std::string &name, StreamBuffer *buf) {
    Content mine;
    if (library.get(name, mine)) {
        std::cout << "ja tenho " << name << "\n";
        if (buf) {
            buf->start(mine.hashes.size(), mine.hashes.size());
        }
        return true;
    }

    // descoberta: pergunta ao tracker quem tem
    std::vector<std::string> t = split(tracker.request("PEERS " + name));
    if (t.size() < 2 || t[0] != "OK") {
        std::cout << "nenhum peer tem " << name << "\n";
        return false;
    }

    // abre uma conexao com cada peer da lista; quem nao responde fica de fora
    std::vector<std::string> sources;
    std::vector<Conn> conns;
    for (size_t i = 1; i < t.size(); i++) {
        // "a.b.c.d:porta" ou "[ipv6]:porta": a porta vem depois do ultimo ':'
        size_t colon = t[i].rfind(':');
        std::string host = t[i].substr(0, colon);
        if (host.size() >= 2 && host.front() == '[') {
            host = host.substr(1, host.size() - 2);
        }
        Conn conn;
        if (conn.connect_to(host, t[i].substr(colon + 1))) {
            // fonte que congela sem fechar a conexao tambem conta como caida
            conn.set_timeout(SOURCE_TIMEOUT);
            sources.push_back(t[i]);
            conns.push_back(conn);
        } else {
            std::cout << "nao consegui conectar em " << t[i] << "\n";
        }
    }
    auto close_all = [&] {
        for (auto &c : conns) {
            c.close_conn();
        }
    };
    if (conns.empty()) {
        return false;
    }

    // tamanho e lista de hashes: pede a primeira fonte; se ela nao manda uma
    // lista com carimbo valido, sai do download e a proxima e tentada
    size_t size = 0;
    std::vector<std::string> hashes;
    while (!conns.empty() && !fetch_info(conns[0], sources[0], name, size, hashes)) {
        conns[0].close_conn();
        conns.erase(conns.begin());
        sources.erase(sources.begin());
    }
    if (conns.empty()) {
        std::cout << "nenhuma fonte mandou uma lista de hashes valida\n";
        return false;
    }
    size_t n_parts = hashes.size();
    std::cout << "baixando " << name << ": " << size << " bytes, " << n_parts << " partes, "
              << sources.size() << " fonte(s)\n";

    // grava num arquivo .part e so troca o nome quando terminar
    std::string final_path = dir + "/" + name;
    std::string part_path = final_path + ".part";
    std::ofstream out(part_path, std::ios::binary);
    if (!out) {
        std::cout << "nao consegui criar " << part_path << "\n";
        close_all();
        return false;
    }
    if (buf) {
        buf->start(n_parts);
    }

    std::mutex out_mtx; // o arquivo e um so para todas as threads
    std::atomic<int> repeated(0);
    std::vector<size_t> got(conns.size(), 0); // partes que vieram de cada fonte

    // a fila de cada fonte: a fonte k comeca com as partes k, k + n_fontes,
    // k + 2*n_fontes, ... e pode ganhar mais se outra fonte cair
    std::mutex todo_mtx;
    std::condition_variable todo_cv;
    std::vector<std::set<size_t>> todo(conns.size());
    std::vector<bool> alive(conns.size(), true);
    size_t left = n_parts; // partes que ainda nao chegaram
    bool failed = false;   // nao sobrou nenhuma fonte
    int moved = 0;         // partes que trocaram de fonte
    for (size_t i = 0; i < n_parts; i++) {
        todo[i % conns.size()].insert(i);
    }

    auto fetch_from = [&](size_t k) {
        while (true) {
            // pega a menor parte da fila; se a fila esta vazia, espera: ainda
            // pode chegar parte de uma fonte que cair
            size_t i;
            {
                std::unique_lock<std::mutex> lock(todo_mtx);
                todo_cv.wait(lock, [&] { return failed || left == 0 || !todo[k].empty(); });
                if (failed || left == 0) {
                    return;
                }
                i = *todo[k].begin();
            }
            // no play, nao pede parte muito a frente do player; se nao coube
            // na janela, volta e olha a fila de novo
            if (buf && !buf->wait_room(i, opt.window)) {
                continue;
            }

            std::string part;
            if (!fetch_part(conns[k], name, i, hashes[i], part, repeated)) {
                // esta fonte sai e as partes dela vao para as que sobraram,
                // uma para cada, alternando
                std::lock_guard<std::mutex> lock(todo_mtx);
                alive[k] = false;
                sources_lost++;
                std::vector<size_t> others;
                for (size_t j = 0; j < conns.size(); j++) {
                    if (alive[j]) {
                        others.push_back(j);
                    }
                }
                if (others.empty()) {
                    printf("fonte %s caiu na parte %zu e nao sobrou nenhuma, download incompleto\n",
                           sources[k].c_str(), i);
                    failed = true;
                    if (buf) {
                        buf->fail();
                    }
                } else {
                    printf("fonte %s caiu na parte %zu, %zu parte(s) repassada(s) para %zu fonte(s)\n",
                           sources[k].c_str(), i, todo[k].size(), others.size());
                    size_t next = 0;
                    for (size_t p : todo[k]) {
                        todo[others[next++ % others.size()]].insert(p);
                        moved++;
                    }
                }
                todo[k].clear();
                todo_cv.notify_all();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(out_mtx);
                out.seekp(i * PART_SIZE);
                out.write(part.data(), part.size());
            }
            got[k]++;
            if (buf) {
                buf->part_arrived(i);
            }
            std::lock_guard<std::mutex> lock(todo_mtx);
            todo[k].erase(i);
            left--;
            todo_cv.notify_all();
        }
    };

    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (size_t k = 0; k < conns.size(); k++) {
        threads.emplace_back(fetch_from, k);
    }
    for (auto &th : threads) {
        th.join();
    }
    close_all();
    out.close();
    if (failed) {
        return false;
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::filesystem::rename(part_path, final_path);
    printf("pronto: %zu bytes em %.3f s (%.1f KB/s), %d parte(s) pedida(s) de novo\n",
           size, secs, size / 1024.0 / secs, repeated.load());
    for (size_t k = 0; k < sources.size(); k++) {
        printf("  %s mandou %zu parte(s)%s\n", sources[k].c_str(), got[k], alive[k] ? "" : " e caiu");
    }
    if (moved > 0) {
        printf("  %d parte(s) trocaram de fonte\n", moved);
    }

    // agora este peer tambem e uma fonte
    library.add(name, final_path);
    std::cout << "tracker: " << tracker.request("ANNOUNCE " + name) << "\n";
    return true;
}

// streaming: baixa numa thread e, nesta, o player simulado assiste as partes
// em ordem, uma a cada 1/taxa segundos
void Peer::play(const std::string &name) {
    using clock = std::chrono::steady_clock;
    auto seconds_since = [](clock::time_point t) {
        return std::chrono::duration<double>(clock::now() - t).count();
    };

    StreamBuffer buf;
    std::thread downloader([&] {
        if (!download(name, &buf)) {
            buf.fail();
        }
    });

    // buffer inicial: so comeca a tocar depois de juntar algumas partes
    auto begin = clock::now();
    bool ok = buf.wait_for(opt.buffer);
    double startup = seconds_since(begin);
    size_t total = buf.parts_total();
    if (ok) {
        printf("player: comecou a tocar depois de %.3f s\n", startup);
    }

    int stalls = 0;
    double stalled = 0; // segundos parado esperando parte
    size_t played = 0;
    auto part_time = std::chrono::duration<double>(1.0 / opt.rate);
    while (ok && played < total) {
        if (buf.parts_ready() <= played) {
            // travada: a parte da vez nao chegou; para e enche o buffer de novo
            printf("player: travou na parte %zu\n", played);
            auto stop = clock::now();
            ok = buf.wait_for(played + opt.buffer);
            stalls++;
            stalled += seconds_since(stop);
            if (!ok) {
                break;
            }
        }
        printf("player: tocando parte %zu/%zu (%zu no buffer)\n",
               played + 1, total, buf.parts_ready() - played - 1);
        std::this_thread::sleep_for(part_time);
        played++;
        buf.set_played(played); // abre espaco na janela do download
    }
    downloader.join();

    if (!ok) {
        printf("player: reproducao interrompida na parte %zu, o download falhou\n", played);
    }
    printf("player: %zu de %zu partes tocadas em %.3f s; espera inicial %.3f s; "
           "%d travada(s), %.3f s parado\n",
           played, total, seconds_since(begin), startup, stalls, stalled);
}

void Peer::run() {
    std::thread(&Peer::heartbeat, this).detach();
    std::thread(&Peer::accept_loop, this).detach();

    std::cout << "comandos: list | peers <conteudo> | get <conteudo> | play <conteudo> | stats | quit\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string cmd;
        if (line == "list") {
            cmd = "LIST";
        } else if (line.compare(0, 6, "peers ") == 0) {
            cmd = "PEERS " + line.substr(6);
        } else if (line.compare(0, 4, "get ") == 0) {
            download(line.substr(4));
            continue;
        } else if (line.compare(0, 5, "play ") == 0) {
            play(line.substr(5));
            continue;
        } else if (line == "stats") {
            printf("enviei:  %ld parte(s), %ld bytes\n", parts_sent.load(), bytes_sent.load());
            printf("recebi:  %ld parte(s), %ld bytes\n", parts_received.load(), bytes_received.load());
            printf("falhas:  %ld parte(s) errada(s), %ld fonte(s) caida(s), %ld lista(s) recusada(s)\n",
                   parts_bad.load(), sources_lost.load(), lists_refused.load());
            cmd = "STATS";
        } else if (line == "quit") {
            break;
        } else if (line.empty()) {
            continue;
        } else {
            std::cout << "comando desconhecido\n";
            continue;
        }
        std::string resp = tracker.request(cmd);
        if (resp.empty()) {
            std::cout << "perdi a conexao com o tracker\n";
            break;
        }
        std::cout << resp << "\n";
    }

    tracker.request("QUIT");
    tracker.close_conn();
}

int main(int argc, char *argv[]) {
    Options options;
    unsigned seed = time(NULL) ^ getpid();

    int opt;
    while ((opt = getopt(argc, argv, "i:c:s:d:r:b:w:k:")) != -1) {
        if (opt == 'i') {
            options.interval = atoi(optarg);
        } else if (opt == 'c') {
            options.corrupt = atof(optarg);
        } else if (opt == 's') {
            seed = atoi(optarg);
        } else if (opt == 'd') {
            options.delay_ms = atoi(optarg);
        } else if (opt == 'r') {
            options.rate = atof(optarg);
        } else if (opt == 'b') {
            options.buffer = atoi(optarg);
        } else if (opt == 'w') {
            options.window = atoi(optarg);
        } else if (opt == 'k') {
            options.key = optarg;
        }
    }
    // a janela nao pode ser menor que o buffer, senao o player esperaria
    // partes que o download nao tem permissao de pedir
    if (options.window < options.buffer) {
        options.window = options.buffer;
    }
    if (options.window < 1) {
        options.window = 1;
    }
    if (argc - optind != 4 || options.rate <= 0) {
        std::cerr << "uso: " << argv[0]
                  << " [-i intervalo_ping] [-c prob_corrupcao] [-s semente] [-d atraso_ms]"
                     " [-r partes_por_segundo] [-b partes_no_buffer] [-w janela] [-k chave]"
                     " <host_tracker> <porta_tracker> <minha_porta> <pasta>\n";
        return 1;
    }
    srand(seed);
    signal(SIGPIPE, SIG_IGN);

    Peer peer(atoi(argv[optind + 2]), argv[optind + 3], options);
    if (!peer.start_listening()) {
        return 1;
    }
    if (!peer.join(argv[optind], argv[optind + 1])) {
        return 1;
    }
    peer.run();
    return 0;
}
