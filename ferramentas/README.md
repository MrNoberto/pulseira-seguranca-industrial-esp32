# Ferramentas

Scripts em Python usados para testar o projeto no PC, sem precisar das placas. Rode a partir da raiz do repositório.

| Script | O que faz | Precisa de |
|---|---|---|
| [`simular.py`](simular.py) | Testa a lógica dos três firmwares, portada linha a linha: formato dos pacotes, alarme de temperatura, cálculo da vibração, zona de perigo, botão BOOT, bipes e LEDs. Lê os parâmetros direto dos `.ino`. | Python 3 |
| [`ajuste_zona.py`](ajuste_zona.py) | Compara 8 formas de suavizar a força do sinal da zona de perigo, com 20 caminhadas simuladas cada, e gera a tabela que justificou a configuração escolhida. | Python 3 |
| [`telas.py`](telas.py) | Desenha as telas da TFT e da OLED pixel a pixel como na placa, usando as fontes reais das bibliotecas, e salva as imagens. | Python 3 + Pillow, bibliotecas Adafruit GFX e U8g2 instaladas |

```bash
python3 ferramentas/simular.py                                  # sai com 0 se tudo passar
python3 ferramentas/ajuste_zona.py                              # imprime a tabela em Markdown
python3 ferramentas/telas.py ~/Arduino/libraries docs/imagens   # gera as imagens das telas
```

As funções portadas para Python precisam ser atualizadas junto com o firmware quando a lógica mudar. Os parâmetros (limites, tempos e pesos) já são lidos automaticamente dos sketches.
