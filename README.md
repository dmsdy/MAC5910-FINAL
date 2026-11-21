# Streaming multimídia P2P — MAC5910

Trabalho final de MAC5910 (IME-USP): uma plataforma distribuída de streaming
em C++, com um servidor de registro (`tracker`) e vários `peer`s que trocam
conteúdo diretamente entre si, sobre TCP. Um vídeo é dividido em partes de
64 KB; quem assiste baixa as partes de várias fontes ao mesmo tempo e as
consome em taxa fixa num player simulado, que conta as travadas.

```
                 +-----------+
                 |  tracker  |   quem está na rede e quem tem cada conteúdo
                 +-----------+
                  /         \
        controle /           \ controle
                /             \
          +--------+       +--------+
          | peer A | <---> | peer B |   partes do conteúdo, direto entre peers
          +--------+       +--------+
```

O que o sistema faz:

- **registro e descoberta**: o peer se registra, anuncia os arquivos da sua pasta e pergunta quem tem um conteúdo;
- **sinal de vida**: o tracker tira da rede o peer que fica calado;
- **integridade**: cada parte é conferida por SHA-256 e pedida de novo se chegar errada;
- **streaming**: buffer inicial, reprodução em taxa fixa, contagem de travadas;
- **várias fontes**: as partes são divididas entre todos os peers que têm o conteúdo;
- **controle de taxa**: o download fica no máximo uma janela de partes à frente do player;
- **tolerância a falhas**: se uma fonte cai ou congela, as outras assumem as partes dela;
- **segurança**: a lista de hashes vem carimbada com HMAC-SHA256 e uma chave compartilhada;
- **IPv4 e IPv6**: tracker e peers atendem os dois na mesma porta;
- **monitoramento**: contadores no peer e no tracker (`stats`).

## Como rodar

Precisa de `g++` com C++17 e da biblioteca do OpenSSL (`libssl-dev`).

```
make
```

Isso gera `tracker` e `peer`.

```
./tracker [porta] [tempo_limite]
./peer [opcoes] <host_tracker> <porta_tracker> <minha_porta> <pasta>
```

Exemplo com duas fontes e um peer que assiste, cada um num terminal:

```
mkdir /tmp/p1 /tmp/p2 /tmp/p3
cp video.mp4 /tmp/p1/
cp video.mp4 /tmp/p2/

./tracker 7000
./peer -k segredo 127.0.0.1 7000 6001 /tmp/p1
./peer -k segredo 127.0.0.1 7000 6002 /tmp/p2
./peer -k segredo 127.0.0.1 7000 6003 /tmp/p3
```

No último:

```
list                 conteúdos disponíveis na rede
peers video.mp4      quem tem o conteúdo
play video.mp4       baixa e assiste ao mesmo tempo
get video.mp4        só baixa
stats                o que este peer enviou e recebeu, e o estado do tracker
quit
```

Ao terminar, o peer que baixou passa a ser fonte também.

O endereço do tracker pode ser IPv4, IPv6 ou um nome: `./peer -k segredo ::1 7000 6003 /tmp/p3`.

### Opções do peer

| Opção | Padrão | Significado |
|---|---|---|
| `-k chave` | vazia | chave compartilhada do grupo |
| `-r taxa` | 1 | partes por segundo que o player consome |
| `-b n` | 3 | partes no buffer antes de começar a tocar |
| `-w n` | 10 | janela: quantas partes o download pode ficar à frente do player |
| `-i s` | 5 | intervalo do sinal de vida, em segundos |
| `-d ms` | 0 | teste: espera antes de enviar cada parte (rede lenta) |
| `-c p` | 0 | teste: probabilidade de corromper uma parte enviada |
| `-s n` | aleatória | teste: semente da corrupção |

Como cada parte tem 64 KB, `-r 1` equivale a um vídeo de cerca de 512 kbit/s.

### Provocando falhas

Com as fontes iniciadas com `-d 500` (2 partes por segundo cada) dá tempo de
mexer durante o `play`:

- `Ctrl+C` numa fonte: as partes dela passam para a outra e a reprodução continua;
- `Ctrl+Z` numa fonte: ela congela; depois de 5 segundos é tratada como caída;
- fonte iniciada com outra chave (`-k outra`): a lista de hashes dela é recusada;
- fonte com `-c 0.2`: 20% das partes chegam erradas e são pedidas de novo.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `tracker.cpp` | servidor de registro: classe `Tracker`, uma thread por conexão |
| `peer.cpp` | classes `Library` (conteúdos e hashes), `StreamBuffer` (buffer do player), `TrackerClient` e `Peer` |
| `conn.h` | classe `Conn`: conexão TCP com leitura por linha e por quantidade de bytes |
| `bench/` | scripts dos experimentos (`roda_tudo.sh`), resultados (`resultados.csv`) e o vídeo de teste |

## Protocolo

Tudo é texto, uma linha por mensagem, sobre TCP.

Peer e tracker:

| Comando | Resposta |
|---|---|
| `REGISTER <porta>` | `OK registered <ip>:<porta>` |
| `ANNOUNCE <conteudo>` | `OK announced <conteudo>` |
| `LIST` | `OK <conteudo> ...` |
| `PEERS <conteudo>` | `OK <ip>:<porta> ...` |
| `PING` | `OK pong` |
| `QUIT` | `OK bye` |
| `STATS` | `OK peers=<n> conteudos=<n> comandos=<n> ligado_ha=<s>s` |

Entre peers:

| Pedido | Resposta |
|---|---|
| `INFO <conteudo>` | `OK <tamanho> <n_partes>`, um SHA-256 por linha e, por último, o HMAC da lista |
| `GET <conteudo> <parte>` | `OK <len>` e depois `len` bytes |

Erros voltam como `ERR <CODIGO> <mensagem>`.

## Limitações

- O player é simulado: não decodifica o vídeo, só mede se cada parte chegou a tempo.
- O conteúdo trafega sem criptografia; o HMAC garante autenticidade e integridade, não sigilo.
- O tracker não confere a chave: um peer de fora consegue se registrar, mas ninguém aceita o que ele manda.
- O tracker é um ponto único de falha: sem ele os peers encerram.

## Créditos

O vídeo de teste `bench/sintel_trailer_480p.mp4` é o trailer de *Sintel*, © Blender Foundation, licença CC BY 3.0 (durian.blender.org).
