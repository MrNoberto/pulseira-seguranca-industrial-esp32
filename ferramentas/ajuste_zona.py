#!/usr/bin/env python3
"""
Compara jeitos de suavizar a força do sinal (RSSI) do farol da zona de perigo.

Para cada configuração (de quanto em quanto tempo a pulseira decide, e o peso da média
quando o sinal sobe ou desce), simula 20 caminhadas até o farol e de volta, com o ruído
de 3 dB típico do rádio, e mede:
  - a distância média em que o alarme liga (entra) e desliga (sai), a 0,5 e a 1 m/s;
  - quantas vezes o alarme troca de estado com a pessoa PARADA na borda por 60 s
    (o ideal é 1: entrou e ficou).

Foi com esta tabela que a pulseira ficou com "decide a cada 200 ms, peso 0,5/0,3".

Uso:
    python3 ferramentas/ajuste_zona.py
"""
import math
import random
import statistics as st

R1M, N, HIST, BORDA = -60.0, 2.5, 3.0, 2.0
LIM = R1M - 10 * N * math.log10(BORDA)


def simular(periodo, sobe, desce, caminho, segundos, semente):
    rnd = random.Random(semente)
    rssi, dentro, trocas, soma, n, ultima = None, False, [], 0, 0, 0
    for ms in range(0, int(segundos * 1000), 2):
        t = ms / 1000
        if ms % 100 == 0:   # o farol manda 10 sinais por segundo
            soma += round(R1M - 10 * N * math.log10(caminho(t)) + rnd.gauss(0, 3))
            n += 1
        if ms - ultima >= periodo:
            ultima = ms
            if n:
                media = soma / n
                soma = n = 0
                if rssi is None:
                    rssi = media
                else:
                    peso = sobe if media > rssi else desce
                    rssi = rssi * (1 - peso) + media * peso
            antes = dentro
            if rssi is not None:
                if rssi >= LIM:
                    dentro = True
                elif rssi < LIM - HIST:
                    dentro = False
            if dentro != antes:
                trocas.append((t, dentro))
    return trocas


def caminhada(v):
    ida = 5.5 / v
    return lambda t: max(0.5, 6 - v * t) if t < ida + 2 else min(6, 0.5 + v * (t - ida - 2))


def medir(periodo, sobe, desce):
    res = {}
    for v in (0.5, 1.0):
        cam, seg = caminhada(v), 2 * 5.5 / v + 2
        entra, sai = [], []
        for sem in range(20):
            tr = simular(periodo, sobe, desce, cam, seg, sem)
            e = [t for t, d in tr if d]
            s = [t for t, d in tr if not d]
            if e:
                entra.append(cam(e[0]))
            if s:
                sai.append(cam(s[0]))
        res[v] = (st.mean(entra), st.mean(sai))
    borda = st.mean(len(simular(periodo, sobe, desce, lambda t: BORDA, 60, sem)) for sem in range(20))
    return res, borda


print(f"Borda real: {BORDA:g} m ({R1M:g} dBm a 1 m, n = {N:g}, histerese {HIST:g} dB, ruído de 3 dB por sinal)\n")
print("| decide a cada | peso sobe/desce | 0,5 m/s: entra / sai | 1 m/s: entra / sai | trocas parado na borda (60 s) |")
print("|---|---|---|---|---|")
for periodo, sobe, desce in [(500, .5, .5), (500, .7, .3), (200, .5, .5), (200, .5, .3),
                             (100, .3, .3), (100, .4, .3), (100, .5, .3), (100, .5, .5)]:
    r, b = medir(periodo, sobe, desce)
    marca = " **(escolhida)**" if (periodo, sobe, desce) == (200, .5, .3) else ""
    print(f"| {periodo} ms | {sobe:.1f} / {desce:.1f}{marca} | {r[0.5][0]:.1f} m / {r[0.5][1]:.1f} m "
          f"| {r[1.0][0]:.1f} m / {r[1.0][1]:.1f} m | {b:.1f} |")
