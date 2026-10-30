#!/usr/bin/env python3
"""
Desenha no PC as telas do projeto, sem precisar gravar nas placas:
  - TFT 1.8" (160x128, Adafruit GFX) do ESP2 - motor
  - OLED 0,96" (128x64, U8g2) do ESP1 - pulseira

As funções tela_motor() e tela_pulseira() são cópias, linha a linha, das funções
desenharTela() dos sketches. As fontes são lidas das próprias bibliotecas instaladas
(Adafruit GFX: arquivos .h; U8g2: u8g2_fonts.c, com o descompactador de glifos da U8g2
reescrito em Python), então o resultado é igual pixel a pixel ao da placa.

Uso:
    python3 ferramentas/telas.py ~/Arduino/libraries docs/imagens

Precisa de Python 3 com Pillow e das bibliotecas "Adafruit GFX Library" e "U8g2".
"""
import math
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

LIB, SAIDA = sys.argv[1], sys.argv[2]
os.makedirs(SAIDA, exist_ok=True)


def round_c(v):  # roundf() do C (metade para longe do zero)
    return math.floor(v + 0.5) if v >= 0 else -math.floor(-v + 0.5)


# =====================================================================
# Adafruit GFX (TFT)
# =====================================================================
def carregar_gfx(nome):
    t = open(f"{LIB}/Adafruit_GFX_Library/Fonts/{nome}.h", encoding="latin-1").read()
    bm = re.search(r"Bitmaps\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", t, re.S).group(1)
    gl = re.search(r"Glyphs\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", t, re.S).group(1)
    ff = re.search(r"GFXfont\s+" + nome + r"\s+PROGMEM\s*=\s*\{(.*?)\};", t, re.S).group(1).split(",")
    return {
        "bitmaps": [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", bm)],
        "glyphs": [tuple(map(int, g)) for g in re.findall(
            r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", gl)],
        "first": int(ff[2], 0), "last": int(ff[3], 0), "yadv": int(ff[4], 0),
    }


_t = open(f"{LIB}/Adafruit_GFX_Library/glcdfont.c", encoding="latin-1").read()
CLASSICA = [int(v, 16) for v in re.findall(
    r"0x[0-9A-Fa-f]+", re.search(r"font\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", _t, re.S).group(1))]
SANS18, SANS9 = carregar_gfx("FreeSansBold18pt7b"), carregar_gfx("FreeSansBold9pt7b")


class Canvas16:
    """O mesmo que GFXcanvas16: cores RGB565, mesmos algoritmos de linha, círculo e texto."""

    def __init__(s, w, h):
        s.w, s.h = w, h
        s.px = [[0] * w for _ in range(h)]
        s.fonte, s.cx, s.cy, s.cor = None, 0, 0, 0xFFFF

    def drawPixel(s, x, y, c):
        if 0 <= x < s.w and 0 <= y < s.h:
            s.px[y][x] = c

    def fillRect(s, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                s.drawPixel(xx, yy, c)

    def fillScreen(s, c):
        s.fillRect(0, 0, s.w, s.h, c)

    def drawFastHLine(s, x, y, w, c):
        s.fillRect(x, y, w, 1, c)

    def drawFastVLine(s, x, y, h, c):
        s.fillRect(x, y, 1, h, c)

    def drawLine(s, x0, y0, x1, y1, c):
        steep = abs(y1 - y0) > abs(x1 - x0)
        if steep:
            x0, y0, x1, y1 = y0, x0, y1, x1
        if x0 > x1:
            x0, x1, y0, y1 = x1, x0, y1, y0
        dx, dy = x1 - x0, abs(y1 - y0)
        err, ystep = dx // 2, (1 if y0 < y1 else -1)
        while x0 <= x1:
            s.drawPixel(*((y0, x0) if steep else (x0, y0)), c)
            err -= dy
            if err < 0:
                y0 += ystep
                err += dx
            x0 += 1

    def drawCircle(s, x0, y0, r, c):
        f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
        for p in ((x0, y0 + r), (x0, y0 - r), (x0 + r, y0), (x0 - r, y0)):
            s.drawPixel(*p, c)
        while x < y:
            if f >= 0:
                y -= 1
                ddy += 2
                f += ddy
            x += 1
            ddx += 2
            f += ddx
            for p in ((x0 + x, y0 + y), (x0 - x, y0 + y), (x0 + x, y0 - y), (x0 - x, y0 - y),
                      (x0 + y, y0 + x), (x0 - y, y0 + x), (x0 + y, y0 - x), (x0 - y, y0 - x)):
                s.drawPixel(*p, c)

    def fillCircleHelper(s, x0, y0, r, cantos, delta, c):
        f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
        px, py, delta = x, y, delta + 1
        while x < y:
            if f >= 0:
                y -= 1
                ddy += 2
                f += ddy
            x += 1
            ddx += 2
            f += ddx
            if x < y + 1:
                if cantos & 1:
                    s.drawFastVLine(x0 + x, y0 - y, 2 * y + delta, c)
                if cantos & 2:
                    s.drawFastVLine(x0 - x, y0 - y, 2 * y + delta, c)
            if y != py:
                if cantos & 1:
                    s.drawFastVLine(x0 + py, y0 - px, 2 * px + delta, c)
                if cantos & 2:
                    s.drawFastVLine(x0 - py, y0 - px, 2 * px + delta, c)
                py = y
            px = x

    def fillCircle(s, x0, y0, r, c):
        s.drawFastVLine(x0, y0 - r, 2 * r + 1, c)
        s.fillCircleHelper(x0, y0, r, 3, 0, c)

    def fillRoundRect(s, x, y, w, h, r, c):
        r = min(r, min(w, h) // 2)
        s.fillRect(x + r, y, w - 2 * r, h, c)
        s.fillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, c)
        s.fillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, c)

    def setFont(s, f):
        s.fonte = f

    def setTextColor(s, c):
        s.cor = c

    def setCursor(s, x, y):
        s.cx, s.cy = x, y

    def print(s, txt):
        for ch in txt:
            s.write(ord(ch))

    def write(s, c):
        if s.fonte is None:  # fonte clássica 5x7 (6 px por letra)
            if s.cx + 6 > s.w:
                s.cx, s.cy = 0, s.cy + 8
            for i in range(5):
                linha = CLASSICA[c * 5 + i]
                for j in range(8):
                    if linha & 1:
                        s.drawPixel(s.cx + i, s.cy + j, s.cor)
                    linha >>= 1
            s.cx += 6
            return
        f = s.fonte
        if f["first"] <= c <= f["last"]:
            bo, w, h, xa, xo, yo = f["glyphs"][c - f["first"]]
            if w > 0 and h > 0:
                if s.cx + xo + w > s.w:
                    s.cx, s.cy = 0, s.cy + f["yadv"]
                bits = bit = 0
                for yy in range(h):
                    for xx in range(w):
                        if not (bit & 7):
                            bits = f["bitmaps"][bo]
                            bo += 1
                        bit += 1
                        if bits & 0x80:
                            s.drawPixel(s.cx + xo + xx, s.cy + yo + yy, s.cor)
                        bits = (bits << 1) & 0xFF
            s.cx += xa

    def getTextBounds(s, txt, x, y):
        minx, miny, maxx, maxy, f = s.w, s.h, -1, -1, s.fonte
        for ch in txt:
            c = ord(ch)
            if f["first"] <= c <= f["last"]:
                bo, gw, gh, xa, xo, yo = f["glyphs"][c - f["first"]]
                if x + xo + gw > s.w:
                    x, y = 0, y + f["yadv"]
                minx, miny = min(minx, x + xo), min(miny, y + yo)
                maxx, maxy = max(maxx, x + xo + gw - 1), max(maxy, y + yo + gh - 1)
                x += xa
        return minx, miny, maxx - minx + 1, maxy - miny + 1


def RGB565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


COR_FUNDO, COR_BARRA, COR_LINHA = RGB565(8, 10, 14), RGB565(26, 32, 42), RGB565(48, 56, 68)
COR_TEXTO, COR_TEXTO_FRACO = RGB565(236, 240, 244), RGB565(132, 142, 154)
COR_VERDE, COR_AMARELO = RGB565(64, 214, 120), RGB565(255, 190, 40)
COR_VERMELHO, COR_CINZA = RGB565(255, 64, 52), RGB565(112, 118, 128)


def formatarGraus(v):
    return f"{v:.0f}" if abs(v - round_c(v)) < 0.05 else f"{v:.1f}"


def formatarLigado(seg):
    h, m, s = seg // 3600, (seg // 60) % 60, seg % 60
    if h < 10:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{h}h{m:02d}m" if h < 100 else f"{h // 24}d{h % 24:02d}h"


# ---- cópia de desenharGrafico(), textoSituacao(), corSituacao() e desenharTela() do esp2_motor.ino ----
def desenharGrafico(c, m, x0, gy, larguraTotal, gh, corLinha):
    gx, gw = x0 + 20, larguraTotal - 20
    val = [v for v in m["hist"] if not math.isnan(v)]
    c.drawFastHLine(gx, gy + gh, gw, COR_LINHA)
    c.drawFastVLine(gx - 2, gy, gh + 1, COR_LINHA)
    c.setFont(None)
    c.setTextColor(COR_TEXTO_FRACO)
    if len(val) < 2:
        c.setCursor(gx + 4, gy + gh // 2 - 4)
        c.print("grafico: 15 min")
        return
    vMin, vMax = math.floor(min(val)), math.ceil(max(val))
    if vMax - vMin < 4:
        meio = round_c((vMax + vMin) / 2)
        vMin, vMax = meio - 2, meio + 2
    t = f"{vMax:.0f}"
    c.setCursor(gx - 4 - len(t) * 6, gy)
    c.print(t)
    t = f"{vMin:.0f}"
    c.setCursor(gx - 4 - len(t) * 6, gy + gh - 7)
    c.print(t)
    L = m["limite"]
    if vMin <= L <= vMax:
        y = gy + gh - int((L - vMin) / (vMax - vMin) * gh + 0.5)
        for x in range(gx, gx + gw, 4):
            c.drawFastHLine(x, y, 2, COR_VERMELHO)
    xAnt = yAnt = -1
    inicio = gx + gw - len(m["hist"])
    for i, v in enumerate(m["hist"]):
        if math.isnan(v):
            xAnt = -1
            continue
        x, y = inicio + i, gy + gh - int((v - vMin) / (vMax - vMin) * gh + 0.5)
        if xAnt >= 0:
            c.drawLine(xAnt, yAnt, x, y, corLinha)
        else:
            c.drawPixel(x, y, corLinha)
        xAnt, yAnt = x, y
    if xAnt >= 0:
        c.fillCircle(xAnt, yAnt, 2, corLinha)


def textoSituacao(m):
    if not m["dhtPronto"]:
        return "INICIANDO"
    if m["aTemp"] and m["aVib"]:
        return "TEMP+VIB"
    if m["aTemp"]:
        return "TEMP ALTA"
    if m["aVib"]:
        return "VIBRACAO"
    return "ERRO SENSOR" if not m["dhtOk"] else "NORMAL"


def corSituacao(m):
    if not m["dhtPronto"]:
        return COR_CINZA
    if m["aTemp"] or m["aVib"]:
        return COR_VERMELHO
    return COR_AMARELO if not m["dhtOk"] else COR_VERDE


def tela_motor(m):
    c = Canvas16(160, 128)
    pisca = (m["agora"] // 500) % 2 == 0
    if not m["dhtPronto"]:
        corTemp = COR_CINZA
    elif not m["dhtOk"]:
        corTemp = COR_AMARELO
    else:
        corTemp = COR_VERMELHO if m["aTemp"] else COR_VERDE
    tP, vP = m["aTemp"] and pisca, m["aVib"] and pisca
    c.fillScreen(COR_FUNDO)
    # barra de cima: motor e tempo ligado
    c.fillRect(0, 0, 160, 13, COR_BARRA)
    c.setFont(None)
    c.setTextColor(COR_TEXTO)
    c.setCursor(4, 3)
    c.print(f"MOTOR {m['id']:02d}")
    t = "LIGADO " + formatarLigado(m["agora"] // 1000)
    c.setTextColor(COR_TEXTO_FRACO)
    c.setCursor(160 - 4 - len(t) * 6, 3)
    c.print(t)
    # bloco da temperatura
    yB, altB = 16, 44
    corValor, corRotulo = corTemp, COR_TEXTO_FRACO
    if tP:
        c.fillRect(0, yB, 160, altB, COR_VERMELHO)
        corValor = corRotulo = COR_TEXTO
    c.setTextColor(corRotulo)
    c.setCursor(6, yB + 4)
    c.print("TEMPERATURA")
    s = textoSituacao(m)
    wP = len(s) * 6 + 8
    xP = 160 - 6 - wP
    if tP:
        c.fillRoundRect(xP, yB + 2, wP, 11, 3, COR_TEXTO)
        c.setTextColor(COR_VERMELHO)
    else:
        c.fillRoundRect(xP, yB + 2, wP, 11, 3, corSituacao(m))
        c.setTextColor(COR_FUNDO)
    c.setCursor(xP + 4, yB + 4)
    c.print(s)
    t = f"{m['temp']:.1f}" if m["dhtOk"] else "--.-"
    c.setFont(SANS18)
    c.setTextColor(corValor)
    dx, _, wValor, _ = c.getTextBounds(t, 0, 0)
    yValor = yB + 38
    c.setCursor(6 - dx, yValor)
    c.print(t)
    xGrau = 6 + wValor + 6
    c.drawCircle(xGrau, yValor - 21, 2, corValor)
    c.drawCircle(xGrau, yValor - 21, 3, corValor)
    c.setFont(SANS9)
    c.setCursor(xGrau + 5, yValor - 11)
    c.print("C")
    # coluna da direita
    c.setFont(None)
    c.setTextColor(COR_TEXTO if tP else COR_TEXTO_FRACO)
    if not m["dhtPronto"]:
        linhas = ["LENDO O", "SENSOR..."]
    elif not m["dhtOk"]:
        linhas = ["VER FIOS", "DO SENSOR"]
    else:
        linhas = [f"UMID {m['umid']:.0f}%"] if not math.isnan(m["umid"]) else []
        if m["minmax"]:
            linhas += [f"MAX {m['tmax']:.1f}", f"MIN {m['tmin']:.1f}"]
    yL, passo = (yB + 16, 9) if len(linhas) == 3 else (yB + 18, 11)
    for i, l in enumerate(linhas):
        c.setCursor(160 - 6 - len(l) * 6, yL + i * passo)
        c.print(l)
    # linha da vibração
    yV, altV = yB + altB + 1, 13
    if vP:
        c.fillRect(0, yV, 160, altV, COR_VERMELHO)
    c.setTextColor(COR_TEXTO if vP else COR_TEXTO_FRACO)
    c.setCursor(6, yV + 3)
    if not m["mpuOk"]:
        c.print("VIB")
        c.setTextColor(COR_AMARELO)
        c.setCursor(36, yV + 3)
        c.print("SEM SENSOR")
    elif math.isnan(m["vib"]):
        c.print("VIB --")
    else:
        c.print(f"VIB {m['vib']:.2f}g")
        xB, wB, yBa, hB = 70, 160 - 6 - 70, yV + 3, 7
        wCh = int(min(m["vib"] / (m["limVib"] * 1.5), 1.0) * wB + 0.5)
        corB = COR_TEXTO if vP else (COR_VERMELHO if m["vib"] >= m["limVib"] else COR_VERDE)
        c.fillRect(xB, yBa, wB, hB, COR_FUNDO if vP else COR_LINHA)
        c.fillRect(xB, yBa, wCh, hB, corB)
        c.drawFastVLine(xB + int(wB / 1.5 + 0.5), yBa - 2, hB + 4, COR_TEXTO)
    # gráfico e rodapé
    yG = yV + altV + 4
    desenharGrafico(c, m, 4, yG, 150, 128 - yG - 14, corTemp if m["dhtOk"] else COR_CINZA)
    c.setFont(None)
    c.setCursor(4, 119)
    if m["envioOk"]:
        c.setTextColor(COR_TEXTO_FRACO)
        c.print(f"LIMITES: {formatarGraus(m['limite'])}C e {m['limVib']:.2f}g")
    else:
        c.setTextColor(COR_VERMELHO)
        c.print("ERRO NO RADIO")
    return c


# =====================================================================
# U8g2 (OLED)
# =====================================================================
U8SRC = open(f"{LIB}/U8g2/src/clib/u8g2_fonts.c", encoding="latin-1").read()


class Leitor:
    """Leitor de bits da U8g2 (u8g2_font_decode_get_unsigned_bits / _signed_bits)."""

    def __init__(s, d, p):
        s.d, s.p, s.bp = d, p, 0

    def u(s, cnt):
        val, n = s.d[s.p] >> s.bp, s.bp + cnt
        if n >= 8:
            s.p += 1
            val = (val | (s.d[s.p] << (8 - s.bp))) & 0xFF
            n -= 8
        s.bp = n
        return val & ((1 << cnt) - 1)

    def s(s, cnt):
        return s.u(cnt) - (1 << (cnt - 1))


class FonteU8:
    def __init__(s, nome):
        m = re.search(r"const uint8_t " + nome + r"\[\d+\][^=]*=\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+);", U8SRC)
        literais = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(1))  # cada literal separado, como no C
        s.d = b"".join(l.encode("latin-1").decode("unicode_escape").encode("latin-1") for l in literais)
        d = s.d
        s.b0, s.b1, s.bw, s.bh, s.bx, s.by, s.bd = d[2], d[3], d[4], d[5], d[6], d[7], d[8]
        s.posA, s.posa = (d[17] << 8) | d[18], (d[19] << 8) | d[20]

    def glifo(s, enc):
        p = 23 + (s.posa if enc >= 97 else s.posA if enc >= 65 else 0)
        while s.d[p + 1] != 0:
            if s.d[p] == enc:
                return p + 2
            p += s.d[p + 1]
        return None

    def info(s, enc):
        p = s.glifo(enc)
        if p is None:
            return None
        L = Leitor(s.d, p)
        w = L.u(s.bw)
        L.u(s.bh)
        xo = L.s(s.bx)
        L.s(s.by)
        return w, xo, L.s(s.bd)

    def largura(s, txt):  # u8g2_GetStrWidth
        w = dx = gw = gxo = 0
        for ch in txt:
            i = s.info(ord(ch))
            if i is None:
                dx = 0
            else:
                gw, gxo, dx = i
            w += dx
        return w - dx + gw + gxo if gw else w

    def desenhar(s, tela, x, y, enc):  # u8g2_font_decode_glyph (modo transparente)
        p = s.glifo(enc)
        if p is None:
            return 0
        L = Leitor(s.d, p)
        w, h = L.u(s.bw), L.u(s.bh)
        gx, gy, d = L.s(s.bx), L.s(s.by), L.s(s.bd)
        if w > 0:
            tx, ty, lx, ly = x + gx, y - (h + gy), 0, 0
            while True:
                a, b = L.u(s.b0), L.u(s.b1)
                while True:
                    for cnt, frente in ((a, False), (b, True)):
                        while True:
                            rem = w - lx
                            if frente:
                                for i in range(min(cnt, rem)):
                                    tela.pixel(tx + lx + i, ty + ly)
                            if cnt < rem:
                                break
                            cnt -= rem
                            lx = 0
                            ly += 1
                        lx += cnt
                    if L.u(1) == 0:
                        break
                if ly >= h:
                    break
        return d


class Mono:
    """Buffer 128x64 de 1 bit com as primitivas da U8g2 usadas no sketch."""

    def __init__(s):
        s.px, s.cor, s.fonte = [[0] * 128 for _ in range(64)], 1, None

    def pixel(s, x, y):
        if 0 <= x < 128 and 0 <= y < 64:
            s.px[y][x] = (s.px[y][x] ^ 1) if s.cor == 2 else s.cor

    def drawHLine(s, x, y, w):
        for i in range(w):
            s.pixel(x + i, y)

    def drawBox(s, x, y, w, h):
        for yy in range(y, y + h):
            s.drawHLine(x, yy, w)

    def drawCircle(s, x0, y0, r):
        f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r

        def secao(x, y):
            for p in ((x0 + x, y0 - y), (x0 + y, y0 - x), (x0 - x, y0 - y), (x0 - y, y0 - x),
                      (x0 + x, y0 + y), (x0 + y, y0 + x), (x0 - x, y0 + y), (x0 - y, y0 + x)):
                s.pixel(*p)

        secao(x, y)
        while x < y:
            if f >= 0:
                y -= 1
                ddy += 2
                f += ddy
            x += 1
            ddx += 2
            f += ddx
            secao(x, y)

    def setFont(s, f):
        s.fonte = f

    def setDrawColor(s, c):
        s.cor = c

    def getStrWidth(s, t):
        return s.fonte.largura(t)

    def drawStr(s, x, y, t):
        for ch in t:
            x += s.fonte.desenhar(s, x, y, ord(ch))


F6X10 = FonteU8("u8g2_font_6x10_tf")
LOGI28 = FonteU8("u8g2_font_logisoso28_tn")
HELVB12 = FonteU8("u8g2_font_helvB12_tr")
AGU, NOR, ALA, SEM, ERR = range(5)   # EstadoPulseira


def formatarMetros(m):
    m = min(m, 99.0)
    return f"{m:.0f}m" if (m >= 9.95 or abs(m - round_c(m)) < 0.05) else f"{m:.1f}m"


# ---- cópia de desenharTela() do esp1_pulseira.ino ----
def tela_pulseira(p):
    o, e, pisca = Mono(), p["estado"], (p["agora"] // 500) % 2 == 0
    # cabeçalho: motor, distância até a zona e sinal do motor
    o.setFont(F6X10)
    o.drawStr(0, 10, f"MOTOR {p['id']:02d}" if p["id"] > 0 else "MOTOR --")
    if p["zonaVista"]:
        t = "ZONA " + formatarMetros(p["dzona"])
        w = o.getStrWidth(t)
        xz = 110 - w
        if p["naZona"]:
            o.drawBox(xz - 2, 0, w + 4, 12)
            o.setDrawColor(0)
            o.drawStr(xz, 10, t)
            o.setDrawColor(1)
        else:
            o.drawStr(xz, 10, t)
    barras, r = 0, p["rssi"]
    if p["fresco"] and e not in (AGU, SEM):
        barras = 4 if r >= -60 else 3 if r >= -70 else 2 if r >= -80 else 1
    for i in range(4):
        alt, x = 3 + i * 2, 113 + i * 4
        if i < barras:
            o.drawBox(x, 10 - alt, 3, alt)
        else:
            o.drawHLine(x, 9, 3)
    o.drawHLine(0, 13, 128)
    # temperatura grande
    temValor = not math.isnan(p["temp"]) and p["jaRecebeu"] and (not p["fresco"] or p["dhtOk"])
    t = f"{p['temp']:.1f}" if temValor else "--.-"
    o.setFont(HELVB12)
    wC = o.getStrWidth("C")
    o.setFont(LOGI28)
    wN = o.getStrWidth(t)
    x = (128 - (wN + 9 + wC)) // 2
    o.drawStr(x, 48, t)
    o.drawCircle(x + wN + 4, 22, 2)
    o.setFont(HELVB12)
    o.drawStr(x + wN + 9, 32, "C")
    # linha de baixo
    o.setFont(F6X10)

    def centro(txt):
        o.drawStr((128 - o.getStrWidth(txt)) // 2, 62, txt)

    if p.get("msg"):
        centro(p["msg"])
    elif e == ALA:
        if pisca:
            o.drawBox(0, 52, 128, 12)
            o.setDrawColor(0)
        aT, aV, z = p["aTemp"], p["aVib"], p["naZona"]
        if z and (aT or aV):
            centro("ZONA + MOTOR!")
        elif z:
            centro("ZONA DE PERIGO!")
        elif aT and aV:
            centro("TEMP E VIBRACAO!")
        elif aT:
            centro("TEMPERATURA ALTA!")
        else:
            centro("VIBRACAO ALTA!")
        o.setDrawColor(1)
    elif e == NOR:
        centro(f"NORMAL  VIB {p['vib']:.2f}g" if p["vibOk"] else "NORMAL  VIB --")
    elif e == SEM:
        seg = p.get("semSinalSeg", 15)
        centro(f"SEM SINAL HA {seg}s" if seg < 60 else f"SEM SINAL HA {seg // 60}min")
    elif e == ERR:
        centro("ERRO SENSOR TEMP")
    else:
        centro("PROCURANDO O MOTOR")
    return o


# =====================================================================
# Cenários e imagens
# =====================================================================
def img_tft(c, escala=3):
    im = Image.new("RGB", (160, 128))
    im.putdata([((v >> 11 & 31) << 3, (v >> 5 & 63) << 2, (v & 31) << 3) for row in c.px for v in row])
    return im.resize((160 * escala, 128 * escala), Image.NEAREST)


def img_oled(o, escala=3):
    im = Image.new("RGB", (128, 64))
    im.putdata([(225, 240, 255) if v else (0, 0, 0) for row in o.px for v in row])
    return im.resize((128 * escala, 64 * escala), Image.NEAREST)


nan = float("nan")
hist_normal = [24.2 + 0.6 * math.sin(i / 9) for i in range(118)]
hist_quente = [24 + max(0, i - 50) * 0.2 for i in range(118)]
motor = dict(id=1, limite=35.0, limVib=0.50, envioOk=True, dhtPronto=True, dhtOk=True, temp=24.6, umid=52,
             tmin=23.6, tmax=24.9, minmax=True, hist=hist_normal, mpuOk=True, vib=0.03, aTemp=False, aVib=False,
             agora=2 * 3600000 + 754000)
TFT = [
    ("tft-normal", "TFT do motor - tudo normal", motor),
    ("tft-vibracao", "TFT - vibração alta (piscando)", dict(motor, vib=0.83, aVib=True)),
    ("tft-temperatura", "TFT - temperatura alta (piscando)",
     dict(motor, temp=37.4, tmax=37.4, hist=hist_quente, aTemp=True)),
    ("tft-erro-dht", "TFT - DHT11 sem resposta", dict(motor, dhtOk=False, umid=nan, hist=hist_normal[:100] + [nan] * 18)),
]
pulseira = dict(estado=NOR, id=1, rssi=-48, temp=24.6, dhtOk=True, vib=0.03, vibOk=True, jaRecebeu=True, fresco=True,
                aTemp=False, aVib=False, zonaVista=True, dzona=4.2, naZona=False, agora=10000)
OLED = [
    ("oled-normal", "OLED da pulseira - tudo normal", pulseira),
    ("oled-zona", "OLED - dentro da zona de perigo", dict(pulseira, estado=ALA, dzona=1.4, naZona=True)),
    ("oled-temperatura", "OLED - temperatura alta", dict(pulseira, estado=ALA, temp=37.4, aTemp=True)),
    ("oled-vibracao", "OLED - vibração alta", dict(pulseira, estado=ALA, aVib=True, vib=0.83)),
    ("oled-zona-motor", "OLED - zona + alarme do motor", dict(pulseira, estado=ALA, dzona=1.6, naZona=True, aTemp=True, temp=37.4)),
    ("oled-calibrado", "OLED - calibração pelo botão BOOT", dict(pulseira, msg="CALIBRADO EM 2m", dzona=2.0)),
    ("oled-sem-sinal", "OLED - motor sem sinal", dict(pulseira, estado=SEM, fresco=False)),
    ("oled-procurando", "OLED - ligando, procurando o motor", dict(pulseira, estado=AGU, id=0, jaRecebeu=False, fresco=False, temp=nan, zonaVista=False)),
]

imagens = []
for nome, legenda, m in TFT:
    im = img_tft(tela_motor(m))
    im.save(f"{SAIDA}/{nome}.png")
    imagens.append((legenda, im))
for nome, legenda, p in OLED:
    im = img_oled(tela_pulseira(p))
    im.save(f"{SAIDA}/{nome}.png")
    imagens.append((legenda, im))

# folha com todas as telas (fonte do sistema com acentos; se não houver, usa a padrão)
fonte = None
for caminho in ("/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "C:/Windows/Fonts/arial.ttf"):
    if os.path.exists(caminho):
        fonte = ImageFont.truetype(caminho, 18)
        break
if fonte is None:
    fonte = ImageFont.load_default(size=20)
colunas, larg, margem = 2, 500, 20
alturas = [im.height + 40 for _, im in imagens]
linhas = [max(alturas[i:i + colunas]) for i in range(0, len(imagens), colunas)]
folha = Image.new("RGB", (colunas * larg + margem, sum(linhas) + margem), (30, 33, 40))
d = ImageDraw.Draw(folha)
y = margem
for i in range(0, len(imagens), colunas):
    for j, (legenda, im) in enumerate(imagens[i:i + colunas]):
        x = margem + j * larg
        d.text((x, y), legenda, fill=(235, 238, 242), font=fonte)
        folha.paste(im, (x, y + 30))
    y += linhas[i // colunas]
folha.save(f"{SAIDA}/telas.png")
print(f"{len(imagens)} telas salvas em {SAIDA}/ (e a folha telas.png)")
