# Como se aproximar do Agbau: Bela e Arduino saem, um ESP32-S3 fica

O [Agbau](https://www.instagram.com/bongarbit/) é outro instrumento do
Bongarbit: 16 botões (matriz 4×4) disparam até 16 vozes polifônicas com
envelope ADSR, hoje divididas entre um Arduino Pro Micro (lê os botões, roda
tap-tempo e um looper com overdub, fala MIDI USB) e uma Bela (recebe esse MIDI
e toca os samples). A decisão tomada em conversa foi: sem Arduino, sem MIDI —
um único ESP32-S3 lê os botões e toca o áudio.

Este documento ordena o trabalho de chegar lá a partir do que já existe nesta
branch, e nomeia o que ainda não tem resposta.

## Por que a base daqui serve

Nada no Agbau processa áudio de entrada; é disparo e soma de sample com
envelope de amplitude, sem filtro, reverb, pitch ou FFT (`agbau/Bela/
agbau-sampler-poly/render.cpp:215-226`, `Sampler.cpp:108-126`). É uma carga de
DSP muito menor que o pitch shifter que já roda nesta branch. A parte de
maior risco técnico — um motor em bloco, float, sem alocação, residente em
PSRAM, aguentando tempo real no S3 — já está provada aqui. O que falta é
engenharia de funcionalidade, não pesquisa de arquitetura.

## Ordem recomendada

| # | Etapa | Por que nessa posição |
|---|---|---|
| 1 | Envelope ADSR em `AudioBlock` | Sem ele nenhuma voz do Agbau se comporta como deveria; é a base de tudo abaixo |
| 2 | Polifonia 16 vozes + seleção aleatória de take + modo loop (ganzás) | Depende só do ADSR; é o `Sampler.cpp` do Agbau reescrito na forma voice-outer já usada aqui |
| 3 | Varredura de matriz 4×4 | Generaliza o `TriggerInput` existente; precisa estar pronta antes de medir latência de disparo real |
| 4 | Orçamento de PSRAM para as samples do Agbau | Decide se a etapa 2 cabe como está ou precisa cortar material primeiro |
| 5 | Mapa de pinos no N16R8 | Decide se a etapa 3 + pedal + LEDs cabem sem conflitar com SD/I2S |
| 6 | Porte do looper (tap-tempo, overdub) | Maior e mais arriscado; só vale a pena depois que 1–5 tocam som de verdade |
| 7 | Placa física | Fora do firmware; pode andar em paralelo a qualquer momento depois do 5 |

### 1. Envelope ADSR

Hoje `src/audio_block.h` tem o joelho suave mas nenhum envelope de amplitude.
O `ADSR.cpp/h` do Agbau (`agbau/Bela/agbau-sampler-poly/ADSR.h`) é o clássico
attack/decay/sustain/release por amostra — portável quase 1:1 para um
processador `process(AudioBlock&)` com estado por voz, seguindo o mesmo
contrato dos processadores já existentes (`PitchShift`, bypass bit-exact
quando não há nota ativa). Precisa de teste equivalente a
`test_bypass_is_bit_exact` e de um que confirme a curva contra os parâmetros
do Agbau (baixos: `0.01, 0.0, 1.0, 1.0`; percussão: `0.01, 3.0, 0.0, 3.0`;
ganzás: `0.01, 0.0, 1.0, 0.1` com loop).

### 2. Dezesseis vozes

A estrutura voice-outer de `audioUpdate()` já soma vozes concorrentes; o
trabalho é ter 16 instâncias de voz (sample + ponteiro de leitura + ADSR) em
vez de 4, cada uma com:
- lista de arquivos candidatos e escolha aleatória a cada disparo (como
  `p1.3.1.wav` … `p1.3.5.wav`);
- flag de loop-enquanto-pressionado para as duas vozes de ganzá;
- flag de "release no note-off" independente por voz (a maioria das
  percussões do Agbau ignora o note-off e deixa o decay correr sozinho).

### 3. Varredura de matriz 4×4

O `TriggerInput` já faz a parte que importa: ler o pino bruto, confirmar por
tempo, rearmar por nível contínuo, sem debounce de biblioteca. Uma matriz
adiciona só o laço de estrobar linha a linha e ler colunas — os diodos já
existem na placa do Agbau para evitar fantasmas (`Circuito/Agbaixo_BOM.csv`:
16 diodos). Cada célula da matriz continua precisando de uma instância de
`TriggerInput` (ou equivalente) por botão: dezesseis, não uma.

### 4. Orçamento de PSRAM

As 24 samples do Agbau somam **~18 MB em float32 mono 44,1 kHz**
(`agbau/Bela/agbau-sampler-poly/samples/`). Convertidas para 16-bit pelo
mesmo caminho de `tools/wav_to_header.py`/`Wav::load`, caem para **~9 MB** —
ainda acima dos 8 MB de PSRAM do N16R8, mesmo sem nenhuma outra alocação.
Os dois maiores arquivos (`p3.1.wav`, `p7.1.wav`, ~3,5 MB cada em float32,
~20 s de loop de ganzá) dominam essa conta. Três saídas, não exclusivas:
cortar cauda/silêncio de cada sample, reduzir samplerate desses dois loops
específicos, ou reduzir o número de takes aleatórios por percussão. Decidir
isso **antes** da etapa 2, porque muda quantas vozes cabem.

### 5. Mapa de pinos

O firmware atual já ocupa 12 GPIOs: SD em HSPI (10/11/12/13), I2S
(16/17/18), disparo (4/5/6), FX (7), probe ADC (1). Esses quatro últimos são
específicos do sampler atual e ficam livres para o Agbau. Uma matriz 4×4 (8
pinos) + botão de modo + pedal + 2 LEDs soma 12 pinos — cabe no que resta do
N16R8 depois de excluir PSRAM octal (33–37), flash (26–32), strapping
(0/3/45/46) e USB nativo (19/20), mas por pouco. Desenhar o mapa explícito
antes de fiar qualquer coisa.

### 6. O looper

O `.ino` do Agbau tem ~465 linhas incluindo tap-tempo e um looper de MIDI com
overdub (3 camadas, até 64 mensagens por camada, LED de status) — e o
próprio README do Agbau documenta bugs conhecidos nesse overdub (contagem de
botões inconsistente entre camadas, note-off disparando cedo demais). Portar
isso é reescrever uma máquina de estado de sequenciamento, não só ligar um
botão a uma voz. Vale fazer **depois** que o resto já produz som, e vale a
chance de corrigir os bugs já documentados em vez de portá-los junto.

### 7. Placa física

`Circuito/Agbaixo_PCB.kicad_pcb` é um carrier para o Arduino Pro Micro: dois
conectores de 12 pinos no footprint do Pro Micro, confirmado pelo BOM. Sem o
Arduino essa placa não serve como está. Duas saídas: fiação solta de um dev
board ESP32-S3 nesses mesmos headers (suficiente para bancada/prototipagem),
ou uma revisão de PCB nova desenhada para o módulo do S3. Não bloqueia nada
do firmware — só precisa existir antes de o instrumento sair da bancada.

## Fora de escopo aqui

Residência em PSRAM, SD intocado após o boot, `audioUpdate()` sem alocação —
nenhuma dessas decisões muda. O Agbau é outro conjunto de vozes e outro
conjunto de botões sobre o mesmo motor, não um motor novo.
