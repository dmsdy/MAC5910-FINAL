#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

exp, comando, fontes, atraso, corrupcao, taxa, rodada, video = sys.argv[1:9]
here = os.path.dirname(os.path.abspath(__file__))
tracker = os.path.join(here, "..", "tracker")
peer = os.path.join(here, "..", "peer")
PORTA = 17000
quieto = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

pasta = tempfile.mkdtemp(prefix="bench_")
processos = [subprocess.Popen([tracker, str(PORTA)], **quieto)]
time.sleep(0.2)

# as fontes: cada uma com uma copia do video
for k in range(1, int(fontes) + 1):
    dir_k = os.path.join(pasta, f"fonte{k}")
    os.mkdir(dir_k)
    shutil.copy(video, os.path.join(dir_k, "video.mp4"))
    semente = str(int(rodada) * 10 + k)
    processos.append(subprocess.Popen(
        [peer, "-k", "bench", "-d", atraso, "-c", corrupcao, "-s", semente,
         "127.0.0.1", str(PORTA), str(PORTA + k), dir_k],
        stdin=subprocess.PIPE, **quieto))
time.sleep(0.5)

# o peer medido; o quit so e lido depois que o get/play termina
dir_c = os.path.join(pasta, "cliente")
os.mkdir(dir_c)
saida = subprocess.run(
    [peer, "-k", "bench", "-r", taxa, "127.0.0.1", str(PORTA), str(PORTA + 99), dir_c],
    input=f"{comando} video.mp4\nquit\n", capture_output=True, text=True, timeout=300).stdout

for p in processos:
    p.kill()
    p.wait()
shutil.rmtree(pasta)

# o que interessa esta em duas linhas da saida do peer
completou, tempo, vazao, repetidas = 0, "", "", saida.count("chegou errada")
m = re.search(r"pronto: \d+ bytes em ([\d.]+) s \(([\d.]+) KB/s\)", saida)
if m:
    completou, tempo, vazao = 1, m.group(1), m.group(2)
caidas = saida.count(" caiu na parte ")
espera, travadas, parado = "", "", ""
m = re.search(r"espera inicial ([\d.]+) s; (\d+) travada\(s\), ([\d.]+) s parado", saida)
if m:
    espera, travadas, parado = m.groups()

print(f"{exp},{comando},{fontes},{atraso},{corrupcao},{taxa},{rodada},{completou},"
      f"{tempo},{vazao},{repetidas},{caidas},{espera},{travadas},{parado}")
