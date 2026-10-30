#!/usr/bin/env python3
"""
Testa a lógica dos três firmwares no PC, sem placas.

As funções de decisão dos sketches (alarme de temperatura, cálculo da vibração, zona de
perigo, botão BOOT, bipes e LEDs) foram portadas linha a linha para Python. Os limites e
parâmetros são lidos direto dos arquivos .ino, então os testes acompanham o código.

Uso (na raiz do repositório):
    python3 ferramentas/simular.py

Sai com código 0 se todas as verificações passarem.
"""
import math
import random
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
E1 = (RAIZ / "esp1_pulseira" / "esp1_pulseira.ino").read_text(encoding="utf-8")
E2 = (RAIZ / "esp2_motor" / "esp2_motor.ino").read_text(encoding="utf-8")
E3 = (RAIZ / "esp3_zona" / "esp3_zona.ino").read_text(encoding="utf-8")
nan = float("nan")


def const(src, nome):
    """Lê 'const tipo NOME = valor;' ou '#define NOME valor' de um sketch."""
    m = re.search(r"\b" + nome + r"\s*=\s*(-?[0-9.]+)", src) or re.search(r"#define\s+" + nome + r"\s+(-?[0-9.]+)", src)
    return float(m.group(1))


# ---- parâmetros lidos dos sketches ----
LIM_T, HIST_T = const(E2, "LIMITE_ALARME"), const(E2, "HISTERESE")
FALHAS = int(const(E2, "FALHAS_ATE_ERRO"))
LIM_V = const(E2, "LIMITE_VIBRACAO")
LIGAR_V, DESLIGAR_V = int(const(E2, "SEGUNDOS_PARA_LIGAR")), int(const(E2, "SEGUNDOS_PARA_DESLIGAR"))
SEM_SINAL = int(const(E1, "TEMPO_SEM_SINAL_MS"))
RSSI_1M, N_AMB = const(E1, "RSSI_1M_PADRAO"), const(E1, "N_AMBIENTE")
HIST_Z, DIST0, SEM_ZONA = const(E1, "HISTERESE_ZONA_DB"), const(E1, "DISTANCIA_PADRAO_M"), int(const(E1, "TEMPO_SEM_ZONA_MS"))
DISTS = [float(v) for v in re.search(r"DISTANCIAS_M\[\]\s*=\s*\{([^}]*)\}", E1).group(1).split(",")]
F_BIP, F_ZONA = int(const(E1, "FREQ_BIP")), int(const(E1, "FREQ_ZONA"))
PERIODO_Z = int(re.search(r"ultimaMediaZona < (\d+)", E1).group(1))
PESO_SOBE, PESO_DESCE = map(float, re.search(r"peso = \(media > rssiZona\) \? ([0-9.]+)f : ([0-9.]+)f", E1).groups())

falhas = 0


def checa(nome, cond, extra=""):
    global falhas
    falhas += 0 if cond else 1
    print(("  OK     " if cond else "  FALHOU ") + nome + (f"   [{extra}]" if extra else ""))


# =====================================================================
# 0) Pacotes: mesmo formato em quem envia e em quem recebe
# =====================================================================
TAM = {"uint32_t": 4, "uint16_t": 2, "uint8_t": 1, "int8_t": 1, "float": 4}


def campos(src, nome):
    corpo = re.search(r"typedef struct __attribute__\(\(packed\)\) \{([^}]*)\} " + nome + ";", src).group(1)
    return re.findall(r"^\s*(\w+)\s+(\w+);", re.sub(r"//.*", "", corpo), re.M)


print("0) Formato dos pacotes ESP-NOW")
pm1, pm2 = campos(E1, "PacoteMotor"), campos(E2, "PacoteMotor")
pz1, pz3 = campos(E1, "PacoteZona"), campos(E3, "PacoteZona")
checa("PacoteMotor igual na pulseira e no motor", pm1 == pm2, f"{sum(TAM[t] for t, _ in pm2)} bytes, {len(pm2)} campos")
checa("PacoteZona igual na pulseira e no farol", pz1 == pz3, f"{sum(TAM[t] for t, _ in pz3)} bytes")
checa("tamanhos diferentes (a pulseira separa os pacotes pelo tamanho)",
      sum(TAM[t] for t, _ in pm1) != sum(TAM[t] for t, _ in pz1))
v = re.findall(r"#define PACOTE_VERSAO (\d+)", E1 + E2) + re.findall(r"#define ZONA_VERSAO (\d+)", E1 + E3)
checa("versões iguais dos dois lados", v[0] == v[1] and v[2] == v[3], f"motor v{v[0]}, zona v{v[2]}")


# =====================================================================
# 1) ESP2: alarme de temperatura (lerTemperatura)
# =====================================================================
class Temperatura:
    def __init__(s):
        s.falhas, s.T, s.alarme, s.dhtOk, s.dhtPronto = 0, nan, False, False, False

    def ler(s, leitura):  # leitura = °C ou None (falha do DHT11)
        if leitura is not None:
            s.falhas, s.T = 0, leitura
        elif s.falhas < 255:
            s.falhas += 1
        tem = not math.isnan(s.T)
        s.dhtOk = tem and s.falhas < FALHAS
        s.dhtPronto = tem or s.falhas >= FALHAS
        if not s.dhtOk:
            s.alarme = False
        elif s.T >= LIM_T:
            s.alarme = True
        elif s.T <= LIM_T - HIST_T:
            s.alarme = False
        return s.alarme


print(f"\n1) ESP2 - temperatura (liga em {LIM_T:g} °C, desliga em {LIM_T - HIST_T:g} °C, erro após {FALHAS} falhas)")
t = Temperatura()
L = LIM_T
roteiro = [(L - 10, False, "bem abaixo"), (L - 0.5, False, "quase no limite"), (L, True, "chegou no limite"),
           (L - 0.5, True, "caiu um pouco: histerese segura o alarme"), (L - HIST_T, False, "caiu 1 °C: desliga"),
           (L - 0.5, False, "subiu de novo, mas abaixo do limite"), (L + 1, True, "passou do limite"),
           (None, True, "1ª falha do DHT11: repete a última leitura"), (None, True, "2ª falha"),
           (None, False, "3ª falha: sensor com erro, alarme sai"), (L + 2, True, "sensor voltou quente")]
for leitura, esperado, descricao in roteiro:
    r = t.ler(leitura)
    rot = "falha" if leitura is None else f"{leitura:g} °C"
    checa(f"{rot:>8} -> alarme={'SIM' if r else 'não'}: {descricao}", r == esperado)


# =====================================================================
# 2) ESP2: vibração (amostrarVibracao + fecharJanelaVibracao)
# =====================================================================
class Vibracao:
    def __init__(s):
        s.alarme, s.acima, s.abaixo, s.ok, s.vib = False, 0, 0, False, nan

    def janela(s, amostras):
        n = len(amostras)
        s.ok = n >= 100
        if s.ok:
            v = 0.0
            for eixo in range(3):
                soma = sum(a[eixo] for a in amostras)
                somaQ = sum(a[eixo] * a[eixo] for a in amostras)
                media = soma / n
                v += somaQ / n - media * media   # variância: tira a gravidade (a média)
            s.vib = math.sqrt(max(v, 0)) / 8192.0
        else:
            s.vib = nan
        if not s.ok:
            s.alarme, s.acima, s.abaixo = False, 0, 0
        elif s.vib >= LIM_V:
            s.abaixo, s.acima = 0, min(s.acima + 1, 255)
            if s.acima >= LIGAR_V:
                s.alarme = True
        elif s.vib < LIM_V * 0.8:
            s.acima, s.abaixo = 0, min(s.abaixo + 1, 255)
            if s.abaixo >= DESLIGAR_V:
                s.alarme = False
        else:
            s.acima = s.abaixo = 0
        return s.vib, s.alarme


def segundo(amp_g, freq=50, buraco_ms=30, n=500, semente=None):
    """1 s de medidas a cada 2 ms (±4 g: 8192 = 1 g). Gravidade no Z, vibração senoidal no X,
    ruído de 5 mg e um buraco no começo (o tempo em que o ESP2 redesenha a tela)."""
    rnd = random.Random(semente)
    cl = lambda g: max(-32768, min(32767, round(g * 8192)))
    out = []
    for i in range(n):
        tt = i * 0.002
        if tt * 1000 < buraco_ms:
            continue
        out.append((cl(amp_g * math.sin(2 * math.pi * freq * tt) + rnd.gauss(0, 0.005)),
                    cl(rnd.gauss(0, 0.005)), cl(1.0 + rnd.gauss(0, 0.005))))
    return out


print(f"\n2) ESP2 - vibração (limite {LIM_V:g} g; liga após {LIGAR_V} s acima; desliga após {DESLIGAR_V} s abaixo de {LIM_V * 0.8:.2f} g)")
vb = Vibracao()
vib, al = vb.janela(segundo(0.0, semente=1))
checa("motor parado: perto de 0 g, sem alarme", vib < 0.02 and not al, f"{vib:.3f} g")
vib, al = vb.janela(segundo(0.3, semente=2))
checa("vibração fraca (0,3 g de pico): RMS certo e sem alarme", abs(vib - 0.3 / math.sqrt(2)) < 0.02 and not al,
      f"{vib:.3f} g; o esperado é 0,3/√2 = {0.3 / math.sqrt(2):.3f}")
vib, al = vb.janela(segundo(1.0, semente=3))
checa("uma batida (1 s forte): ainda sem alarme", vib > LIM_V and not al, f"{vib:.3f} g")
vb.janela(segundo(0.0, semente=4))
a1 = vb.janela(segundo(1.0, semente=5))[1]
vib, al = vb.janela(segundo(1.0, semente=6))
checa("forte por 2 s seguidos: ALARME", not a1 and al, f"{vib:.3f} g")
vib, al = vb.janela(segundo(0.6, semente=7))
checa("entre 80% e 100% do limite: o alarme continua", al and LIM_V * 0.8 <= vib < LIM_V, f"{vib:.3f} g")
res = [vb.janela(segundo(0.1, semente=8 + i))[1] for i in range(3)]
checa("abaixo de 80%: desliga só no 3º segundo", res == [True, True, False], str(res))
vib, al = vb.janela(segundo(0.0, n=40, semente=11))
checa("MPU6050 desconectado (poucas medidas): sem alarme", not vb.ok and not al)
vib, al = vb.janela(segundo(3.0, freq=120, semente=12))
checa("vibração de 3 g a 120 Hz medida sem saturar (±4 g)", 1.9 < vib < 2.3, f"{vib:.3f} g")


# =====================================================================
# 3) ESP1: pulseira (zona, botão BOOT, estado, bipes, LEDs)
# =====================================================================
AGU, NOR, ALA, SEM, ERR = "AGUARDANDO", "NORMAL", "ALARME", "SEM SINAL", "ERRO SENSOR"


class Pulseira:
    def __init__(s):
        s.ms, s.ev, s.log = 0, [], []
        s.buzz, s.bips, s.lig, s.desl, s.freq, s.prox, s.ultimoAlarme = False, 0, 100, 100, F_BIP, 0, 2
        s.jaRecebeu, s.ultimoPacote, s.sensorOk, s.vibOk = False, 0, False, False
        s.aTemp = s.aVib = s.aTempAtivo = s.aVibAtivo = False
        s.estado = AGU
        s.dist, s.rssi1m = DIST0, RSSI_1M
        s.rssiZona, s.zonaVista, s.naZona, s.ultimaZona, s.ultimaMedia, s.somaZ, s.nZ = nan, False, False, 0, 0, 0, 0
        s.botao = s.botaoAntes = s.longa = False
        s.inicioBotao, s.somaCalib, s.nCalib, s.msg = 0, 0.0, 0, ""

    # ---- buzzer (atualizarBuzzer) ----
    def bipar(s, n, a, b, f):
        s.ev.append((s.ms, 0, 0))
        s.buzz, s.bips, s.lig, s.desl, s.freq, s.prox = False, n, a, b, f, s.ms

    def calar(s):
        s.bips, s.buzz = 0, False
        s.ev.append((s.ms, 0, 0))

    def atualizarBuzzer(s, ag):
        if s.estado == ALA and s.bips == 0 and not s.buzz and ag - s.prox >= 0:
            ativos = [s.aTempAtivo, s.aVibAtivo, s.naZona]
            for k in (1, 2, 3):
                i = (s.ultimoAlarme + k) % 3
                if ativos[i]:
                    s.ultimoAlarme = i
                    if i == 0:
                        s.bipar(3, 120, 90, F_BIP)
                    elif i == 1:
                        s.bipar(2, 350, 150, F_BIP)
                    else:
                        s.bipar(5, 60, 60, F_ZONA)
                    break
        if ag - s.prox < 0:
            return
        if s.buzz:
            s.ev.append((ag, 0, 0))
            s.buzz, s.prox = False, ag + (s.desl if s.bips > 0 else 700)
        elif s.bips > 0:
            s.ev.append((ag, 1, s.freq))
            s.buzz, s.bips, s.prox = True, s.bips - 1, ag + s.lig

    # ---- zona (atualizarZona, limiteZona) ----
    def limite(s):
        return s.rssi1m - 10 * N_AMB * math.log10(s.dist)

    def farol(s, rssi):
        s.somaZ += rssi
        s.nZ += 1

    def atualizarZona(s, ag):
        if ag - s.ultimaMedia < PERIODO_Z:
            return
        s.ultimaMedia = ag
        soma, n, s.somaZ, s.nZ = s.somaZ, s.nZ, 0, 0
        if n > 0:
            media = soma / n
            if s.zonaVista and not math.isnan(s.rssiZona):
                peso = PESO_SOBE if media > s.rssiZona else PESO_DESCE
                s.rssiZona = s.rssiZona * (1 - peso) + media * peso
            else:
                s.rssiZona = media
            if s.botaoAntes:
                s.somaCalib += media
                s.nCalib += 1
            s.ultimaZona, s.zonaVista = ag, True
        elif s.zonaVista and ag - s.ultimaZona > SEM_ZONA:
            s.zonaVista, s.rssiZona = False, nan
        antes = s.naZona
        if not s.zonaVista:
            s.naZona = False
        elif s.rssiZona >= s.limite():
            s.naZona = True
        elif s.rssiZona < s.limite() - HIST_Z:
            s.naZona = False
        if s.naZona != antes:
            s.log.append((ag, "ENTROU" if s.naZona else "SAIU"))

    # ---- botão BOOT (trocarDistancia, calibrarZona, atualizarBotao) ----
    def trocarDistancia(s):
        atual = max([i for i, d in enumerate(DISTS) if abs(d - s.dist) < 0.01] or [0])
        s.dist = DISTS[(atual + 1) % len(DISTS)]
        s.msg = f"ZONA DE {s.dist:g}m"
        s.bipar(1, 60, 0, F_ZONA)

    def calibrarZona(s):
        if not s.zonaVista or math.isnan(s.rssiZona):
            s.msg = "SEM SINAL DA ZONA"
            s.bipar(1, 400, 0, F_BIP)
            return
        aqui = s.somaCalib / s.nCalib if s.nCalib >= 3 else s.rssiZona
        s.rssi1m = aqui + 10 * N_AMB * math.log10(s.dist)
        s.msg = f"CALIBRADO EM {s.dist:g}m"
        s.bipar(2, 80, 80, F_ZONA)

    def atualizarBotao(s, ag):
        ap = s.botao
        if ap and not s.botaoAntes:
            s.inicioBotao, s.longa, s.somaCalib, s.nCalib = ag, False, 0.0, 0
        if ap and not s.longa and ag - s.inicioBotao >= 3000:
            s.longa = True
            s.calibrarZona()
        if not ap and s.botaoAntes and not s.longa and ag - s.inicioBotao >= 50:
            s.trocarDistancia()
        s.botaoAntes = ap

    # ---- estado (calcularEstado) e LEDs (atualizarLedsSituacao, modo de 2 LEDs) ----
    def calcularEstado(s, ag):
        fresco = s.jaRecebeu and ag - s.ultimoPacote <= SEM_SINAL
        s.aTempAtivo, s.aVibAtivo = fresco and s.aTemp, fresco and s.aVib
        if s.aTempAtivo or s.aVibAtivo or s.naZona:
            return ALA
        if not s.jaRecebeu:
            return AGU
        if not fresco:
            return SEM
        return ERR if not s.sensorOk else NOR

    def leds(s, ag):
        pr, pd = (ag // 250) % 2 == 0, (ag // 500) % 2 == 0
        fresco = s.jaRecebeu and ag - s.ultimoPacote <= SEM_SINAL
        perigo = s.aTempAtivo or s.aVibAtivo or s.naZona
        problema = (not fresco) or (not s.sensorOk) or (not s.vibOk)
        return (not perigo) and (not problema), (pr if perigo else (problema and pd))

    def motor(s, temp_alarme=False, vib_alarme=False, sensor_ok=True, vib_ok=True):
        s.jaRecebeu, s.ultimoPacote = True, s.ms
        s.sensorOk, s.vibOk = sensor_ok, vib_ok
        s.aTemp, s.aVib = sensor_ok and temp_alarme, vib_ok and vib_alarme

    def loop(s):
        ag = s.ms
        s.atualizarZona(ag)
        s.atualizarBotao(ag)
        novo = s.calcularEstado(ag)
        if novo != s.estado:
            antigo, s.estado = s.estado, novo
            if antigo == ALA:
                s.calar()
        s.atualizarBuzzer(ag)
        s.ms += 2


def rssi_real(d, rnd, r1m=RSSI_1M):
    """Modelo de propagação (log-distância) + ruído de 3 dB, em dBm inteiros como o rádio entrega."""
    return round(r1m - 10 * N_AMB * math.log10(d) + rnd.gauss(0, 3))


def rodar(p, seg, rnd, dist=None, motor=None, botao=None, r1m=RSSI_1M):
    """dist: t(s) -> metros (None = farol desligado); motor: kwargs do pacote a cada 1 s; botao: t -> apertado."""
    t0 = p.ms
    while p.ms < t0 + seg * 1000:
        tt = (p.ms - t0) / 1000
        if dist is not None and p.ms % 100 == 0:
            p.farol(rssi_real(dist(tt), rnd, r1m))
        if motor is not None and p.ms % 1000 == 0:
            p.motor(**motor)
        p.botao = botao(tt) if botao else False
        p.loop()


def bipes(p, de, ate):
    out, ini = [], None
    for tt, on, f in p.ev:
        if not de <= tt < ate:
            continue
        if on:
            ini = (tt, f)
        elif ini:
            out.append((tt - ini[0], ini[1]))
            ini = None
    return out


OK = dict()   # pacote do motor sem alarme
print(f"\n3) ESP1 - zona de perigo (borda padrão {DIST0:g} m: {RSSI_1M:g} dBm a 1 m, n = {N_AMB:g}, histerese {HIST_Z:g} dB,"
      f" média a cada {PERIODO_Z} ms com peso {PESO_SOBE:g}/{PESO_DESCE:g})")
rnd = random.Random(7)
ida = lambda v: (lambda tt: max(0.5, 6 - v * tt) if tt < 5.5 / v + 2 else min(6, 0.5 + v * (tt - 5.5 / v - 2)))
for v, minimo in ((0.5, 1.6), (1.0, 1.4)):
    ents, sais, trocas_extras = [], [], 0
    for k in range(20):   # 20 caminhadas com ruído diferente
        r, p, cam = random.Random(200 + k), Pulseira(), ida(v)
        rodar(p, 2 * 5.5 / v + 2, r, dist=cam, motor=OK)
        e = [tt for tt, x in p.log if x == "ENTROU"]
        sa = [tt for tt, x in p.log if x == "SAIU"]
        if e and sa:
            ents.append(cam(e[0] / 1000))
            sais.append(cam(sa[0] / 1000))
        trocas_extras += max(0, len(p.log) - 2)
    me, ms = sum(ents) / max(1, len(ents)), sum(sais) / max(1, len(sais))
    checa(f"andando a {v:g} m/s (20 caminhadas): entra perto da borda, sai um pouco depois, sem liga-desliga",
          len(ents) == 20 and minimo <= me <= 2.4 and ms <= 3.8 and trocas_extras == 0,
          f"entra em média a {me:.2f} m ({min(ents):.1f}-{max(ents):.1f}), sai a {ms:.2f} m; trocas a mais: {trocas_extras}")
p = Pulseira()
rodar(p, 30, rnd, dist=lambda tt: DIST0, motor=OK)
checa("parado exatamente na borda por 30 s: não fica entrando e saindo", len(p.log) <= 2, f"{len(p.log)} trocas")
p = Pulseira()
rodar(p, 4, rnd, dist=lambda tt: 1.0, motor=OK)
a = p.ms
rodar(p, 5, rnd, motor=OK)
checa(f"farol desligado: depois de ~{SEM_ZONA / 1000:g} s sai da zona e para de bipar",
      not p.naZona and not p.zonaVista and p.estado == NOR and not bipes(p, a + SEM_ZONA + 600, p.ms))
p = Pulseira()
rodar(p, 3, rnd, motor=OK)
rodar(p, 12, rnd)
checa("motor parou de mandar: SEM SINAL", p.estado == SEM)
rodar(p, 4, rnd, dist=lambda tt: 1.0)
checa("e a zona continua funcionando sozinha", p.estado == ALA and p.naZona)

print("\n4) ESP1 - bipes de cada alarme (e rodízio quando há mais de um)")


def padrao(motor, dist=None):
    p = Pulseira()
    rodar(p, 6, random.Random(3), dist=dist, motor=motor)
    b = bipes(p, 2000, 6000)
    return [d for d, _ in b], {f for _, f in b}


d, f = padrao(dict(temp_alarme=True))
checa(f"temperatura: trios de 120 ms a {F_BIP} Hz", d[:3] == [120] * 3 and f == {F_BIP}, str(d[:6]))
d, f = padrao(dict(vib_alarme=True))
checa(f"vibração: duplas de 350 ms a {F_BIP} Hz", d[:2] == [350] * 2 and f == {F_BIP}, str(d[:4]))
d, f = padrao(OK, dist=lambda tt: 1.0)
checa(f"zona: cinco de 60 ms a {F_ZONA} Hz (mais agudo)", d[:5] == [60] * 5 and f == {F_ZONA}, str(d[:5]))
d, f = padrao(dict(temp_alarme=True, vib_alarme=True))
checa("temperatura + vibração: alterna trio curto e dupla longa", 120 in d and 350 in d, str(d[:8]))
d, f = padrao(dict(temp_alarme=True), dist=lambda tt: 1.0)
checa("temperatura + zona: alterna trio e cinco rápidos", 120 in d and 60 in d and F_ZONA in f, str(d[:10]))

print("\n5) ESP1 - botão BOOT")
p, seq = Pulseira(), []
for _ in range(len(DISTS)):
    rodar(p, 0.3, rnd, botao=lambda tt: True)
    rodar(p, 0.5, rnd)
    seq.append(p.dist)
i0 = [i for i, x in enumerate(DISTS) if abs(x - DIST0) < 0.01][0]
esperado = [DISTS[(i0 + k) % len(DISTS)] for k in range(1, len(DISTS) + 1)]
checa("toque rápido percorre as distâncias e volta ao início", seq == esperado, " -> ".join(f"{x:g} m" for x in [DIST0] + seq))
p = Pulseira()
r0 = p.rssi1m
rodar(p, 4, rnd, botao=lambda tt: True)
rodar(p, 0.5, rnd)
checa("segurar 3 s sem o farol: avisa e não muda nada", p.msg == "SEM SINAL DA ZONA" and p.rssi1m == r0 and p.dist == DIST0)
p = Pulseira()
rodar(p, 3, rnd, dist=lambda tt: 2.0, r1m=-50)
rodar(p, 4, rnd, dist=lambda tt: 2.0, botao=lambda tt: True, r1m=-50)
checa("segurar 3 s parado a 2 m: calibra a borda ali (o sinal real a 1 m é -50 dBm)",
      abs(p.rssi1m + 50) < 2 and p.msg == "CALIBRADO EM 2m", f"equivale a {p.rssi1m:.1f} dBm a 1 m")


def entradas(borda, toques, inicio, semente0):
    ents = []
    for k in range(20):
        r = random.Random(semente0 + k)
        q = Pulseira()
        for _ in range(toques):
            rodar(q, 0.3, r, botao=lambda tt: True)
            rodar(q, 0.5, r)
        rodar(q, 3, r, dist=lambda tt: borda, r1m=-50)
        rodar(q, 4, r, dist=lambda tt: borda, botao=lambda tt: True, r1m=-50)
        q.log.clear()
        t0 = q.ms
        rodar(q, 4 * inicio, r, dist=lambda tt: max(0.3, inicio - 0.25 * tt), r1m=-50)
        e = [tt for tt, x in q.log if x == "ENTROU"]
        if e:
            ents.append(max(0.3, inicio - 0.25 * ((e[0] - t0) / 1000)))
    return ents


for borda, toques, inicio in ((2.0, 0, 4.0), (3.0, 1, 5.5)):
    ents = entradas(borda, toques, inicio, 100)
    media = sum(ents) / len(ents) if ents else 0
    checa(f"escolheu {borda:g} m, calibrou lá e voltou devagar: entra em média perto de {borda:g} m (20 repetições)",
          len(ents) == 20 and abs(media - borda) <= 0.2 * borda and min(ents) >= 0.7 * borda and max(ents) <= 1.5 * borda,
          f"média {media:.2f} m, de {min(ents):.1f} a {max(ents):.1f} m")

print("\n6) ESP1 - LEDs (modo de 2 LEDs: azul no D25, amarelo no D26)")


def descrever(serie):
    on = sum(serie) / len(serie)
    n = sum(1 for x, y in zip(serie, serie[1:]) if x and not y)
    return "apagado" if on == 0 else "ACESO" if on == 1 else ("pisca rápido" if n >= 3 else "pisca devagar")


casos = [("tudo perfeito", OK, None, "ACESO", "apagado"),
         ("temperatura alta", dict(temp_alarme=True), None, "apagado", "pisca rápido"),
         ("vibração alta", dict(vib_alarme=True), None, "apagado", "pisca rápido"),
         ("dentro da zona", OK, 1.0, "apagado", "pisca rápido"),
         ("DHT11 com erro", dict(sensor_ok=False), None, "apagado", "pisca devagar"),
         ("MPU6050 sem resposta", dict(vib_ok=False), None, "apagado", "pisca devagar"),
         ("motor desligado", None, None, "apagado", "pisca devagar")]
for nome, mot, dz, az_esp, am_esp in casos:
    p = Pulseira()
    rodar(p, 4, random.Random(1), dist=(lambda tt: dz) if dz else None, motor=mot)
    amostra = []
    for _ in range(1000):   # 2 s
        p.loop()
        amostra.append(p.leds(p.ms))
    az, am = descrever([x for x, _ in amostra]), descrever([y for _, y in amostra])
    checa(f"{nome:22} azul {az:13} amarelo {am}", az == az_esp and am == am_esp)

print("\nRESULTADO:", "tudo certo" if falhas == 0 else f"{falhas} falha(s)")
sys.exit(1 if falhas else 0)
