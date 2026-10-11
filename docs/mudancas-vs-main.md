# O que foi mudado desde o estado inicial, e por quê

Este documento nasceu comparando com a `main`, quando ela era um commit único
(`8b66dfc`): um sampler que tocava, com um bug de disparo, ~47 ms de latência e
nenhuma verificação automatizada.

**A `main` andou desde então.** Em 11/10/2026 ela está em `98966ad` e já contém
as áreas **1 a 11** abaixo. O que a branch `feat/mixer-gain-staging` acrescenta
por cima da `main` são as áreas **12, 13 e 14** — o pin da M16, o motor de
blocos em float e o pitch shifter — mais o registro de bancada que as
acompanha.

As áreas 1–11 ficam aqui porque a justificativa delas continua sendo a única
explicação escrita de por que o firmware é como é. Este arquivo é o histórico
de decisões do projeto, não o diff de uma branch.

Resumo das áreas, cada uma justificada abaixo:

| # | Área | O que mudou | Por quê |
|---|---|---|---|
| 1 | Disparo | Rearme lê o pino bruto, não o estado da Bounce2 | Bug: ~112 disparos por batida |
| 2 | Disparo | Um `TriggerInput` para os 4 botões | Duas implementações divergentes esconderam o bug |
| 3 | Latência | Ring de DMA de 46 ms → 5,8 ms | Era 98% do orçamento de latência |
| 4 | Build | `pioarduino` pinado + board `n16r8` | Core 3.x obrigatório; teto de tamanho errado |
| 5 | Log | `DEBUG_BOOT` / `DEBUG_TRIGGERS` separados | Serial no laço de polling trava o disparo |
| 6 | Log | PSRAM reportada no boot | Sem ela, falha de PSRAM parecia problema de cartão |
| 7 | I2S | Canal RX desligado (`din = -1`) | Projeto não tem entrada de áudio |
| 8 | Testes | Suíte de 23 testes no host + simulador | Lógica de disparo era inverificável |
| 9 | Diagnóstico | Probe de latência; sketch de tom I2S | Medir em vez de estimar |
| 10 | Docs | `circuit-rc.md`, `bancada.md`, `CLAUDE.md` | Registrar decisões e medições |
| 11 | Mixer | Ganho por voz, master e joelho suave | Kit normalizado clipava com 2 pads |
| 12 | Build | M16 pinado num commit exato | A biblioteca reescreveu o transporte de áudio sob o projeto |
| 13 | Motor | `audioUpdate()` renderiza blocos em float | Sem bloco não existe janela para DSP com memória |
| 14 | Efeito | Pitch shift no GPIO7 enquanto pressionado | Primeiro processador de stream; GPIO7 deixa de ser pad |

---

## 1. O bug de retrigger

**Antes:** os pads disparavam na leitura bruta do pino (bom, zero latência) mas
**rearmavam** pelo estado *debounced* da Bounce2 (ruim).

**Por que é bug:** o estado debounced atrasa em relação ao pino pelo intervalo
de debounce. Rearmar pelo *nível* redisparava **~112 vezes por batida**;
rearmar pela *borda* (`rose()`) travava o pad permanentemente quando o contato
durava menos que `bounce + interval`. Qualquer condição de rearme lida de um
estado filtrado acopla a correção à constante do filtro.

**Agora:** as duas proteções leem o pino diretamente — confirmação de
`TRIG_CONFIRM_US` (200 µs) para disparar, e pino em alto contínuo por
`TRIG_RELEASE_MS` (3 ms) para rearmar. Nenhuma biblioteca de debounce.

**Medido em hardware (13/08/2026):** 17 batidas → 17 linhas. A `main`, na mesma
placa e na mesma sessão, deu **196**.

## 2. Um `TriggerInput` para os quatro botões

**Antes:** o GPIO4 tinha debounce próprio de "espera estabilizar" com 25 ms; os
pads tinham outro esquema. Duas implementações divergentes no mesmo caminho de
disparo.

**Por que mudou:** essa divergência é o que deixou o bug dos pads sobreviver à
revisão — o botão principal se comportava bem, então o problema parecia ser dos
pads. Além disso, os 25 ms eram a maior parcela de latência do GPIO4.

**Agora:** [src/trigger_input.h](../src/trigger_input.h) é compilado no firmware
**e** nos testes. Divergência passa a ser impossível por construção, não por
vigilância. As duas janelas são membros de instância (não macros lidas inline),
para que varreduras e testes possam variá-las por botão — custa 8 bytes por
botão.

## 3. O ring de DMA: 46 ms → 5,8 ms

**Por que importa:** a M16 alimenta o ring de I2S uma amostra por vez e bloqueia
quando ele está cheio. A profundidade do ring **é** a latência de disparo,
essencialmente toda ela. O default da biblioteca (4 × 512 = 2048 frames = 46 ms)
é dimensionado para sobreviver a um apagamento de setor de flash no host.

**Por que dá para ser mais raso aqui:** esse pior caso não existe neste
firmware. Os samples ficam residentes na PSRAM, o SD não é tocado depois do
boot, não há WiFi e nada escreve em flash em runtime. Sobra o cache miss de
PSRAM dentro do `audioUpdate()`, que é sub-microssegundo.

**Resultado:** o termo que dominava o orçamento de latência era este, e ninguém
tinha olhado para ele. Os 0,2 ms de debounce que se discutia eram 0,4% do
problema.

**Validado em hardware:** limpo com as **quatro vozes soando ao mesmo tempo**,
que é o caminho mais pesado do `audioUpdate()`. Ver
[bancada.md, "Fase E"](bancada.md) — incluindo os dois confundidores que
inverteram a conclusão no meio do caminho.

O `#define` tem de ficar **antes** do `#include "M16.h"`, porque a M16 guarda os
próprios valores com `#ifndef`. Movê-lo para depois restaura silenciosamente os
46 ms, sem nenhum outro sintoma — por isso existe um `static_assert`.

## 4. Build: plataforma e board

**`platform`** passou de `espressif32` para uma release exata do
**pioarduino**. A M16 precisa de `driver/i2s_std.h`, que exige Arduino core 3.x
(ESP-IDF 5); a plataforma oficial não o fornece. Pinado a uma versão exata, não
a uma faixa, porque é a dependência cuja deriva silenciosa seria mais difícil de
depurar a partir dos sintomas de áudio.

**`board`** passou de `esp32-s3-devkitc-1` (que é a variante N8, 8MB de flash e
**sem PSRAM**) para `esp32-s3-devkitc1-n16r8`. Antes, a correção era uma pilha
de overrides `board_build`/`board_upload`. Eles funcionavam — `BOARD_HAS_PSRAM`
era definido e a tabela de partições de 16MB era aplicada — mas deixavam
`maximum_size` no 8MB do N8 **enquanto escreviam uma tabela de partições que vai
até 0xff0000**. O tamanho do firmware era conferido contra o teto errado. A
definição de board correta resolve partições, `memory_type`, `psram_type`,
`BOARD_HAS_PSRAM` e os limites de tamanho de forma coerente e num só lugar.

**Nota:** verificado em 13/08/2026 que `platform = espressif32` pode resolver
para o pacote pioarduino já instalado localmente, dando a impressão de que a
plataforma oficial funciona. Em máquina limpa, provavelmente não.

## 5. Log de boot separado do log de disparo

**Por quê:** Serial sobre USB CDC **bloqueia** quando o host tem a porta aberta
e não está drenando o buffer. Um `printf` dentro do laço de polling põe um
travamento sem limite superior no caminho do disparo.

**Agora:** `DEBUG_BOOT` (só `setup()`, custa zero em runtime, deixe ligado) e
`DEBUG_TRIGGERS` (uma linha por batida, **desligado por padrão**). Erros são
incondicionais nos dois casos.

**Confirmado em hardware:** com `DEBUG_TRIGGERS 1` e a porta aberta sem leitor,
o intervalo entre bater no pad e ouvir o som foi de **2 a 3 segundos** — porque
o `TRIGDBG()` vem **antes** do `start()` no laço. Voltando a `0`, imperceptível.

## 6. PSRAM reportada no boot

**Por quê:** a `psramAllocSafe()` da M16 não tem fallback para RAM interna.
Sem PSRAM, todo `Wav::load()` falha, e o único sintoma era um
`WAV: failed to load` que manda a pessoa depurar a fiação do cartão SD quando o
problema é configuração de board. Agora a ausência de PSRAM é reportada
explicitamente, nomeando a causa provável, antes que qualquer coisa dependa
dela.

**Medido:** 8.386.096 bytes livres de 8.388.608.

## 7. Canal I2S RX desligado

O default da M16 é `i2sPinsOut[] = {16, 17, 18, 21}`, e um `din` diferente de
`-1` faz o `audioStart()` alocar um canal RX, chamar
`i2s_channel_init_std_mode()` uma segunda vez com a mesma `std_cfg` e habilitar
um periférico que ninguém lê. Este projeto não tem entrada I2S — o
[circuit.md](circuit.md) lista o GPIO21 como não conectado desde sempre.

**Honestidade sobre esta mudança:** ela entrou como suspeita de causar um
silêncio total, e **essa suspeita foi descartada** (a `main`, com RX ligado,
toca). Fica por mérito próprio — custo zero, menos um caminho de inicialização
não usado — não como correção de bug.

## 8. Testes no host

**Antes:** nada. A lógica de disparo só podia ser verificada gravando a placa.

**Agora:** `pio test -e native` roda 23 testes em ~2 s, sem placa. A suíte é
organizada por **motivo**, não por forma de código: cada design rejeitado tem um
teste nomeado pela maneira como falhou
(`test_rejected_level_rearm_machine_guns`,
`test_rejected_edge_rearm_latches_on_short_contact`), de modo que reintroduzir
um deles falha um teste que se explica. O
`test_harness_reproduces_historical_machine_gun` fixa o modelo nas contagens
originalmente medidas (112/3) — se ele falhar, o modelo derivou e nenhuma outra
asserção do arquivo significa nada.

`sim/` tem o gerador de traço de pino, o modelo do filtro RC e cópias
**deliberadas** dos dois designs rejeitados, que existem só para continuar
falhando da maneira documentada. Timestamps são `uint32_t`, como `micros()`, de
modo que a aritmética sem sinal estoura na simulação exatamente como na placa.

**Limite explícito:** a suíte verifica disparo. Áudio, SD, residência em PSRAM e
profundidade do ring só se verificam gravando. Um build verde e uma suíte verde
não dizem nada sobre a PSRAM ter inicializado.

## 9. Instrumentos de diagnóstico

- **Probe de latência** (`LATENCY_PROBE`, desligado por padrão): mede a cadeia
  inteira observando a **saída analógica do DAC** por um divisor polarizado num
  pino de ADC. Um marcador em GPIO não serviria — o dreno do DMA e o filtro do
  DAC acontecem *depois* de a CPU escrever, e são justamente os termos a
  calibrar. Ainda não executado.
- **`src/test_i2s_bench.cpp`** (ambiente `i2s_bench`): banco de medição do
  transporte de áudio, falando direto com `i2s_std` — sem M16, sem SdFat. Nasceu
  como gerador de tom para separar "este DAC consegue emitir som" de qualquer
  coisa que o firmware pudesse estar fazendo errado (o mesmo papel que o
  `sd_test` tem para o cartão), e virou o instrumento que decidiu a
  arquitetura. Mede CPU real, jitter e underrun de três modelos de alimentação
  em nove profundidades de ring. Resultados em
  [bancada.md, seção 9](bancada.md).

  O que ele achou: a escrita por amostra da M16 custa **21-34% de um core**, e
  trocar para escrita por bloco recupera quase tudo (**2,3%**); o modelo de ISR
  vai limpo até **0,36 ms** de ring; o despacho de interrupção do IDF custa
  ~1.380 ciclos, o que faz `2 × 16` ser mais barato que `4 × 8` com a mesma
  latência; e quatro vozes lidas da PSRAM pelo mixer custam 110 ciclos/frame,
  confirmando com número o "sub-microssegundo" que o `main.cpp` afirmava.

Um terceiro sketch implica adicionar exclusão em **todos** os ambientes do
`platformio.ini`, não só no novo.

## 10. Documentação

- [circuit-rc.md](circuit-rc.md) — o filtro RC proposto e o rig de medição, com
  as constantes de tempo assimétricas e, explicitamente, **onde o RC piora**
  (ele alonga glitch de contato em ~1081 µs, derrotando a janela de confirmação
  de 200 µs que o rejeitaria sem o filtro).
- [bancada.md](bancada.md) — o roteiro da primeira sessão de hardware e, agora,
  os resultados dela: o que foi validado, os dois defeitos de módulo
  encontrados, as conclusões que se revelaram falsas e por quê.
- [circuit.md](circuit.md) — acrescentado o aviso de que "os pinos de controle
  não estão expostos" **não** significa que estão resolvidos: num módulo
  GY-PCM5102A de 6 pinos eles ficam em jumpers de solda no verso, e podem vir
  todos abertos. `XSMT` aberto = saída muda, sem nenhum sintoma em log.
- [CLAUDE.md](../CLAUDE.md) — as decisões que sustentam o projeto, escritas para
  que a próxima pessoa não as desfaça por parecerem supérfluas.

## 11. Mixer: ganho por voz, master e joelho suave

**Antes:** o `audioUpdate()` somava as vozes e aplicava um clamp duro no fim.

**Por que é problema:** com um kit normalizado — que é o que todo pacote de
samples entrega — isso clipa por construção. Dois samples com pico em 89% do
fundo de escala somam 178%. Clamp duro num transiente percussivo soa como
estalo, e na bancada de 13/08/2026 esses estalos foram confundidos com estouro
de DMA por um bom tempo.

**Agora:** [src/mixer.h](../src/mixer.h), modelado em como uma MPC resolve —
que **não** é dividir pelo número de vozes:

- **Ganho Q8 por voz** (`padGain[]`, `trigGain`) e **ganho master**. Um multiply
  e um shift por voz, sem float no caminho de áudio. Unidade por padrão, então
  ligar não muda nada audível até o kit ser balanceado; existe para que o
  controle exista.
- **Joelho suave** como última linha de defesa:
  `y = KNEE + (RANGE·over)/(RANGE + over)`, identidade abaixo de 75% do fundo de
  escala e assintótico ao teto acima. Nunca clipa, para nenhuma entrada.

**Por que essa curva:** derivada exatamente 1 no joelho (C1-contínua, sem quina
audível onde a compressão começa), assintótica (clipping deixa de ser possível
por construção, não por ajuste), ímpar, sem estado e sem lookahead — ou seja,
**sem latência adicional**, o que importa porque os 5,8 ms validados são a razão
de ser do projeto.

**Alternativas rejeitadas, e o número que decidiu:**

| Abordagem | Por que não |
|---|---|
| Dividir por `PAD_COUNT+1` | −12 dB sempre, para um pior caso que raramente ocorre |
| Ganho dinâmico `1/N` | Pumping: um pad novo abaixa o que já está soando. Em percussão é pior que distorção |
| Limitador com lookahead | Precisa de linha de atraso — mataria os 5,8 ms |
| Limitador sem lookahead | Reage depois do transiente que deveria conter |

O argumento mais forte só apareceu ao escrever os testes: para quatro vozes
ficarem **inteiramente lineares** (abaixo do joelho) cada uma precisaria de
~**−13,7 dB**, *pior* que os −12 dB do ÷4. Headroom linear para o pior caso é
caro; o joelho é o que o devolve.

**Verificação:** 18 testes em `test/test_mixer/`, organizados pela propriedade
que defendem — incluindo um que documenta numericamente a alternativa rejeitada,
para que o tradeoff continue visível se alguém propuser o ÷4 de novo.

**Custo:** +1208 bytes de RAM, +1580 de flash. Dois `#define` desligam cada
camada independentemente (`MIXER_GAIN_STAGING`, `MIXER_SOFT_CLIP`), porque isso
**muda o som do instrumento** e não é decisão para tomar em silêncio.

**Não verificado em hardware ainda.** A aritmética está testada no host; se o
joelho soa bem num rolo de quatro vozes é questão de bancada.

---

## 12. M16 pinada num commit exato

Até 11/10/2026 a dependência era `https://github.com/algomusic/M16.git`, sem
revisão. Isso não é um risco hipotético: **a biblioteca reescreveu o transporte
de áudio enquanto o projeto era construído em cima dela.**

`i2s_write_samples()` fazia uma chamada `i2s_channel_write()` por frame. Hoje é
um shim `[[deprecated]]` sobre `audioBlockWrite()`, que acumula 32 frames e faz
uma escrita de DMA por bloco. Mesma chamada no nosso código, mesmo som — e
~19% de um núcleo de diferença, que é exatamente a grandeza que a seção 9 da
[bancada.md](bancada.md) passou uma sessão medindo.

A mesma atualização acrescentou um modo dual-core *block-split* em que o shim
**retorna `false` e não escreve nada**, alcançável se um default lá em cima
mudar (`setIsDualCore`, hoje `false` no ESP32).

| Consequência | Detalhe |
|---|---|
| Medições expiram em silêncio | Toda nota "validado em hardware" em `docs/` ficava pendurada numa versão que podia trocar |
| O firmware chamava função obsoleta | Agora chama `audioBlockWrite()` direto |
| Um default poderia emudecer a placa | O firmware agora declara `setIsDualCore(false)` em vez de herdar |

**O pin é honesto, não tranquilizador:** ele **não** é o commit contra o qual a
bancada de 13–14/08 rodou. Aquele checkout foi destruído e re-clonado em
10/10/2026 e o commit é irrecuperável (o clone tem uma única entrada de
reflog). Por isso a escada de DMA e o resultado de quatro vozes estão marcados
como **provisórios** até serem reconfirmados nesta versão.

O `CLAUDE.md` já fazia esse argumento para `platform`. A biblioteca de áudio
tem pelo menos o mesmo direito a ele — e agora tem a evidência.

---

## 13. O motor de áudio: blocos em float

`audioUpdate()` produzia um frame por chamada. Agora renderiza
`AUDIO_BLOCK_FRAMES` (32 = 0,73 ms) de float normalizado (±1,0) num bloco e o
drena no fim. Mesmas vozes, mesma mistura, mesma curva de limitador — o motor
embaixo é que mudou.

Três motivos, em ordem crescente de quanto realmente pesaram:

| Motivo | Peso |
|---|---|
| Chamadas de driver (~1.126 ciclos/frame, ~19% de um núcleo) | **Nenhum.** A M16 já passou a agrupar 32 frames sozinha — este ganho já estava no bolso |
| Localidade de PSRAM | **Real.** Voz-por-fora/frame-por-dentro faz cada voz percorrer 32 frames contíguos do próprio buffer, em vez de intercalar quatro streams de PSRAM e sacudir a cache a cada frame |
| **Janela para DSP** | **Foi o que forçou a mudança.** Um pitch shifter precisa de uma janela de sinal, não de uma amostra |

O terceiro é o que o agrupamento da M16 **não** resolve: ela guarda a *nossa
saída*, depois de já termos tido que decidir qual é.

Float porque ±1,0 é o que toda framework de áudio fala (Bela, JUCE, APIs de
plugin, todo paper de DSP). Não é por precisão — ponto fixo de 16 bits dava
conta de somar quatro vozes. É porque a próxima coisa a entrar aqui é um stream
de áudio ao vivo, e uma cadeia em unidades convencionais aceita DSP de qualquer
lugar sem reescalar. A FPU do S3 torna isso quase gratuito **numa task**, e
indisponível **numa ISR** — que é o tradeoff registrado na seção 9 da bancada e
a razão de o motor ser uma task.

O `src/mixer.h` saiu do caminho de áudio e ficou como **referência**:
`test_float_knee_matches_the_integer_reference` varre as duas curvas e exige
concordância dentro de 1 LSB. A curva inteira é a que foi julgada de ouvido na
placa, então esse teste é o que diz que esta mudança **não alterou o som** — e
o que impede as duas de divergirem em silêncio. Não apagar como código morto.

**Verificação:** 16 testes em `test/test_audio_block/`. Em hardware: boot
limpo, `single-core audio task mode`, blocos de 32 frames confirmados no log,
sem watchdog. **Não julgado de ouvido ainda.**

---

## 14. Pitch shift no GPIO7, como processador de stream

Segura o GPIO7 e a mistura inteira cai uma octava; solta e volta. O GPIO7
**deixa de ser pad** — o efeito substituiu o pad, não foi acrescentado ao lado.
`/3.wav` não é mais carregado.

**Escrito para um stream, não para o sampler.** Mudar o pitch de um sample
guardado é trivial: o `Samp` já reamostra, então tocar o buffer mais rápido
mudaria o tom — e encurtaria o som. Isto processa um sinal de que nunca viu o
fim: lê um bloco, escreve um bloco, mantém o próprio histórico, e não sabe de
onde o áudio veio. Quando o audio-in chegar, ele é lido no topo do
`audioUpdate()` e o `pitch_shift.h` **não muda**.

`test_block_size_does_not_change_the_output` é o que faz disso um fato e não uma
esperança: o mesmo sinal em pedaços irregulares de 1, 7, 32, 3, 64, 13, 128 e 5
frames tem que dar saída bit-idêntica a uma única chamada de 16384 frames. Se o
tamanho do bloco do hospedeiro vazasse para o som, um callback de entrada de
tamanho diferente não seria drop-in e a premissa estaria errada.

**O algoritmo:** duas derivações (*taps*) janeladas numa linha de atraso, lidas
a `ratio` enquanto a escrita corre a 1, com crossfade de **potência constante**
para que o ganho de cada tap seja exatamente zero onde ela tem que pular.

Os custos são inerentes a mudar pitch no domínio do tempo, não falhas de
implementação, e documentados em vez de escondidos:

| Artefato | Valor | Observação |
|---|---|---|
| Warble | ~43 Hz numa octava | Percussão mascara; nota tonal sustentada não |
| Latência | ~11,6 ms | É a janela de reamostragem. `latencyFrames()` reporta em vez de deixar descobrir |

Os dois escalam com `PITCH_BUF_FRAMES` em direções opostas — é o único botão
real aqui.

| Alternativa | Por que não |
|---|---|
| Phase vocoder (FFT) | Troca warble por borrão de transiente, precisa de janela **no mínimo tão longa** (logo não é mais rápido) e custa muito mais CPU. Para percussão é o tradeoff errado |
| Interpolação cúbica nos taps | Fica abaixo do warble que o efeito já tem; se o shift precisar soar limpo em material tonal, aí o vocoder é a resposta melhor |
| Tabela de janela em vez de `sinf()` | `sinf()` custa ~4% do orçamento do bloco. É a primeira coisa a fazer se isso precisar ficar mais barato, não antes |

Três decisões que merecem nome:

- **`held()` é um acessor novo no `TriggerInput`, não uma máquina nova.** Segurar
  precisa exatamente do condicionamento que bater precisa, e `armed` já marca
  justamente o intervalo entre um toque confirmado e um soltar acreditado. Um
  debounce separado para o caso de segurar seria a segunda implementação
  divergente no caminho de disparo que o `trigger_input.h` existe para evitar.
- **`setEngaged()` só registra um pedido**; a task de áudio aplica na fronteira
  do bloco. Agir direto poria o `loop()` escrevendo a fase do crossfade
  enquanto a task de áudio a lê — uma corrida real entre núcleos, no único
  estado sobre o qual o algoritmo todo é construído. Custa até 0,73 ms no
  gesto, contra os 11,6 ms que o efeito já carrega.
- **O processador roda sempre**, alimentando a linha de atraso mesmo
  desengatado, senão apertar o botão começa o efeito a partir do silêncio. O
  bypass é bit-exato, o que é o que torna gratuito deixá-lo no caminho.

**Verificação:** 15 testes em `test/test_pitch_shift/`, incluindo medições por
DFT de que a energia realmente vai para a octava acima e abaixo. Em hardware:
log de boot confirma `ratio 0.5000` e 11,6 ms, estável sem watchdog. **Não
julgado de ouvido ainda.**

---

## Mapa do repositório

```
src/main.cpp            o sampler (setup/loop/audioUpdate)
src/trigger_input.h     TriggerInput: update() = batida, held() = segurar
src/mixer.h             ganho e joelho em ponto fixo — REFERÊNCIA dos testes,
                        já fora do caminho de áudio (não apagar)
src/audio_block.h       AudioBlock, unidades float, conversões das pontas
src/pitch_shift.h       PitchShift: dois taps janelados numa linha de atraso
src/test_sd.cpp         diagnóstico de cartão com a SD.h do core (env sd_test)
src/test_i2s_bench.cpp  banco de medição de I/O I2S, sem M16 (env i2s_bench)
sim/trigger_model.h     gerador de traço, modelo RC, designs rejeitados
sim/trigger_sim.cpp     CLI de varredura (--sweep=all)
test/test_trigger/      suíte Unity do disparo, 25 testes
test/test_mixer/        suíte Unity do mixer em ponto fixo, 18 testes
test/test_audio_block/  suíte Unity do motor float, 16 testes
test/test_pitch_shift/  suíte Unity do pitch shifter, 13 testes
                        todas: pio test -e native  (71 casos)
tools/wav_to_header.py  .wav → header C, com a mesma aritmética da M16
docs/                   circuit.md, circuit-rc.md, bancada.md, este arquivo
```

Todo `.h` em `src/` é compilado **no firmware E nos testes**. É a convenção
mais importante do repositório, e existe porque o `TriggerInput` já morou no
`main.cpp` com uma cópia à mão no simulador — e as duas divergiram. DSP novo
nasce header-only e sem Arduino pelo mesmo motivo.

Os ambientes do PlatformIO compartilham `src/` e são separados por
`build_src_filter`, porque cada sketch tem o próprio par `setup()`/`loop()`.

## Estado de verificação

| Item | Como está verificado |
|---|---|
| Lógica de disparo | 25 testes no host, e 17/17 batidas na placa |
| PSRAM, SD, carga dos `.wav` | log de boot em hardware |
| Polifonia (soma de vozes) | hardware |
| Transporte I2S: CPU, jitter, underrun | banco v2 em hardware, 9 profundidades |
| Motor de blocos em float | 16 testes no host + **ouvido na placa** em 11/10/2026 |
| Pitch shifter | 13 testes no host (DFT confirma a octava) + **ouvido na placa** com material real |
| Mixer: joelho suave em colisão | 18 testes no host + **ouvido na placa**: 3 vozes a ~−3 dBFS, soma ~267% do fundo de escala |
| Ring de DMA de 5,8 ms | limpo com 3 vozes nesta versão da M16 |
| Conector USB | **não explicado** — áudio confiável no COM, não no nativo |
| Latência ponta a ponta | **não medida** — fase F, bloqueada até o conector estar entendido |
| Filtro RC | **não montado** — só simulado |
| Rejeição de batida fantasma | **não testada** em hardware |

Duas ressalvas que valem mais que a tabela:

1. **Todo resultado anterior de "não clipa" foi obtido com material que não
   tinha como clipar** — as cópias da escada de DMA somavam ~67% do fundo de
   escala com três vozes. Só em 11/10/2026, com os samples reais (vários a
   0,0 dBFS), o joelho foi exercitado de verdade. E aí sim está confirmado.
2. **O teste de quatro vozes não existe mais neste firmware.** O GPIO7 virou o
   efeito, então o máximo é GPIO4+5+6. Três vozes acima do joelho testam o
   limitador com folga, mas a escada de DMA de agosto foi medida *com quatro* e
   não tem como ser reproduzida sem devolver o pad.
