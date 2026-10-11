# O que esta branch muda em relação à `main`, e por quê

A `main` é um commit único (`8b66dfc`), o estado inicial do projeto: um sampler
que tocava, com um bug de disparo, ~47 ms de latência e nenhuma verificação
automatizada. Esta branch corrige isso e documenta o que foi medido.

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
- **`src/test_i2s_tone.cpp`** (ambiente `i2s_tone`): gera uma onda quadrada
  contínua falando direto com o driver `i2s_std`, sem M16, sem SdFat, sem
  samples — e **confere o retorno de cada chamada**, que a M16 não faz (um
  `i2s_new_channel()` falho é silencioso e o `M16 is running` é impresso de
  qualquer jeito). Serve para separar "este DAC consegue emitir som" de qualquer
  coisa que o firmware possa estar fazendo errado, do mesmo modo que o `sd_test`
  separa o cartão da SdFat. **Compila, nunca foi executado em hardware.**

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

## Mapa do repositório

```
src/main.cpp            o sampler (setup/loop/audioUpdate)
src/trigger_input.h     TriggerInput — compilado no firmware E nos testes
src/mixer.h             ganho por voz, master e joelho suave — idem
src/test_sd.cpp         diagnóstico de cartão com a SD.h do core (env sd_test)
src/test_i2s_tone.cpp   tom contínuo falando direto com i2s_std (env i2s_tone)
sim/trigger_model.h     gerador de traço, modelo RC, designs rejeitados
sim/trigger_sim.cpp     CLI de varredura (--sweep=all)
test/test_trigger/      suíte Unity do disparo, 23 testes
test/test_mixer/        suíte Unity do mixer, 18 testes
                        ambas: pio test -e native
tools/wav_to_header.py  .wav → header C, com a mesma aritmética da M16
docs/                   circuit.md, circuit-rc.md, bancada.md, este arquivo
```

Os ambientes do PlatformIO compartilham `src/` e são separados por
`build_src_filter`, porque cada sketch tem o próprio par `setup()`/`loop()`.

## Estado de verificação

| Item | Como está verificado |
|---|---|
| Lógica de disparo | 23 testes no host, e 17/17 batidas na placa |
| PSRAM, SD, carga dos `.wav` | log de boot em hardware |
| Ring de DMA de 5,8 ms | 4 vozes simultâneas, por ouvido |
| Polifonia (soma de vozes) | hardware |
| Mixer: ganho e joelho suave | 18 testes no host; **não ouvido em hardware** |
| Latência ponta a ponta | **não medida** — fase F pendente |
| Filtro RC | **não montado** — só simulado |
| Rejeição de batida fantasma | **não testada** em hardware |
| Sketch `i2s_tone` | **compila, nunca executado** |
