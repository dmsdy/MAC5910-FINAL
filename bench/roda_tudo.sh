#!/bin/bash
# Roda todos os experimentos do relatorio e junta tudo em resultados.csv

cd "$(dirname "$0")"
VIDEO=$(realpath "${1:-sintel_trailer_480p.mp4}")
RODADAS=5
OUT=resultados.csv
echo "experimento,comando,fontes,atraso_ms,corrupcao,taxa,rodada,completou,tempo_s,vazao_kbs,repetidas,fontes_caidas,espera_inicial_s,travadas,parado_s" > $OUT

run() { python3 experimento.py "$@" "$VIDEO" | tee -a $OUT; }

for r in $(seq 1 $RODADAS); do
    # 1. vazao pelo numero de fontes: sem atraso (limite da maquina) e com
    #    cada fonte segurando 200 ms por parte (5 partes por segundo)
    for n in 1 2 3 4; do
        run vazao get $n 0 0 1 $r
        run vazao get $n 200 0 1 $r
    done

    # 2. streaming: player a 8 partes por segundo, fontes a 5 cada
    for n in 1 2 3 4; do
        run streaming play $n 200 0 8 $r
    done

    # 3. perda: duas fontes estragando uma fracao das partes
    for c in 0 0.05 0.1 0.2 0.3; do
        run perda play 2 200 $c 8 $r
    done
done
