# LAS Recomp para Android

[English](README.md) | **Português**

Um port para Android do **[LAS Recomp](https://github.com/peiikai/LASRecomp)**, de peiikai: uma recompilação
estática de *The Legend of Zelda: Link's Awakening* (Nintendo Switch, 2019). Como no projeto original, isto **não é
emulação**: o código do Switch vira código ARM64 nativo, e os gráficos do jogo (NVN) são traduzidos para Vulkan.
Para as versões de PC (Windows, macOS, Linux), os recursos e o randomizer, veja o
[repositório original](https://github.com/peiikai/LASRecomp).

> [!IMPORTANT]
> Assim como o original, este port foi feito quase inteiramente com IA: começou com Claude (Opus 5.5,
> da Anthropic, via Claude Code), com desenvolvimento posterior auxiliado por **GPT Astra** e **GPT Sol 6.1**
> (OpenAI, via Codex), dirigido, testado e revisado por uma pessoa. Você executa o código por sua conta.

> [!WARNING]
> **Experimental.** Baixe o APK na página de [Releases](https://github.com/mamcjr/LASRecompAndroid/releases): todo o
> resto (os seus arquivos do jogo, o código do jogo, o driver) é configurado pelo próprio app, no celular. O plano
> completo, o estado de cada fase e as medições estão em [docs/ANDROID-PORT.md](docs/ANDROID-PORT.md).

> [!NOTE]
> **Nenhum arquivo do jogo está incluído**, e o app não contém nada do jogo. Você precisa da sua própria cópia,
> obtida legalmente, dos arquivos extraídos de *Link's Awakening* **v1.0.0**. Não serão dadas instruções de como
> obtê-los.

## Novidades da 0.3.8

APK 0.3.8 atualizado: a tela inicial mostra apenas **PLAY**, sem o seletor Vanilla/seed. O novo fundo segue o visual de vila em miniatura do jogo, com um pequeno Android aventureiro.

![Ícone Android aventureiro](docs/icon/android-hero.png)

![Novo fundo da ilha com Android](docs/artwork/android-island.png)

- **Menu touch:** categorias laterais, descrições junto das opções, controles maiores, seletores e rolagem por arraste, com botão fixo para voltar.
- **Otimizações:** reaproveitamento de pipelines, buffers de conversão de texturas e cópias de leitura na arena; comparação exata para texturas estáveis e tradução de shaders compute em fila. Compiladores de shaders configuráveis.
- **ADPF estável:** mantém a medição anterior por tempo decorrido e a sessão sincronizada. No teste do usuário a 50% com Turnip, os intervalos capturados melhoraram de cerca de 52-58 FPS para 58,9-59,9 FPS. Ainda há picos isolados; não é benchmark controlado nem garantia para outros aparelhos.
- **Inicialização após atualização:** ferramentas internas só são extraídas quando o conteúdo muda.
- **Artes novas:** ícone Android aventureiro e fundo original de ilha gerados com OpenAI. São ilustrações de fã, não assets extraídos do jogo.

Validação: build de release no PC, 22 verificações dos caches, 12 verificações simuladas de gestos e 28 testes de transferência de saves. O usuário testou o frametime no aparelho com Turnip. O caminho Qualcomm/Mali e todos os fluxos do menu ainda precisam de validação mais ampla. [Otimizações](docs/ANDROID-OPTIMIZATIONS.md) · [Menu touch](docs/ANDROID-TOUCH-SETTINGS.md).

## Importar e exportar saves

Na tela inicial, abra **Settings → Saves & files → Saves - Eden / Yuzu / Ryujinx**:

- **Import save folder**: selecione a pasta de saves de *Link's Awakening*, que contém `savedata0.bin`
  (ou outro `savedataN.bin`) e os demais arquivos `.bin` do jogo.
- **Import save ZIP**: selecione um ZIP dessa pasta. Uma pasta externa dentro do ZIP é aceita; arquivos com
  saves de mais de um perfil são recusados para evitar misturar progressos.
- **Export saves**: escolha onde salvar o ZIP. Com o jogo fechado no emulador, extraia o conteúdo na pasta
  de saves de *Link's Awakening* aberta pelo próprio Eden, Yuzu ou Ryujinx.
- **Export previous saves**: exporte o backup anterior à última importação. Para restaurá-lo, importe esse ZIP.

A importação substitui todos os slots do conjunto selecionado e mantém uma cópia dos saves anteriores. A
transferência funciona antes de iniciar o jogo. Se o Android não permitir selecionar a pasta de outro app,
exporte os saves pelo emulador para uma pasta acessível, como Documentos, e selecione essa cópia ou ZIP.
São transferidos os arquivos originais `.bin`, sem conversão; dados extras do randomizer não fazem parte do ZIP.

## Capturas de tela

![A tela inicial](docs/android/start_screen.jpg)

| ![Mapa aberto, 58 fps](docs/android/overworld.jpg) | ![Praia de Toronbo Shores, 60 fps](docs/android/beach.jpg) |
|:-:|:-:|
| ![Encontrando a espada](docs/android/sword.jpg) | ![Toronbo Shores](docs/android/shore.jpg) |

Em um POCO F7 Ultra com Turnip: os controles de toque na tela e o overlay de desempenho ligado (canto superior esquerdo).

![Vila Mabe em 75% (1080p), tela cheia, 60 fps](docs/android/1080p_fullscreen.jpg)

Em 75% de resolução interna (1080p) e em tela cheia: 55-60 fps, quase sempre 60.

![Vila Mabe em 75% (1080p), 16:9, 60 fps](docs/android/1080p_16x9.jpg)

Em 75% (1080p) em 16:9, o formato do Switch (barras nas laterais): 60 fps estáveis.

![Vila Mabe na resolução nativa, tela cheia, 30 fps](docs/android/native_fullscreen.jpg)

Na resolução nativa (3200×1440, 100%) e em tela cheia: 30 fps estáveis, no modo de 30 fps do próprio jogo (velocidade
normal).

## Estado atual

Testado em um **POCO F7 Ultra** (Snapdragon 8 Elite / Adreno 830, Android 16) com o driver **Turnip Gen8 V37**:

- o jogo inicia, os vídeos tocam com som, e os menus, os saves e a jogabilidade funcionam;
- **60 fps estáveis**, inclusive no mapa aberto, na resolução interna padrão de 50% (720p): 98% do tempo a 60 nas
  nossas medições no mapa aberto, sem nenhum quadro acima de 18 ms. Em 75% (1080p) segurou 60 em 16:9 e rodou a
  55-60 fps em tela cheia nos nossos testes, e na resolução nativa (3200×1440) em tela cheia a 30 estáveis. O jogo ainda tem o seu próprio modo de 30 fps, como no Switch, para quando um quadro não cabe;
- **controles de toque**: um analógico flutuante, A / B / X / Y, L / R / ZL / ZR, Menos / Mais e um botão de menu,
  exibidos enquanto nenhum controle está conectado (somem assim que um é conectado). O layout pode ser editado
  (arrastar para onde quiser, tamanho, opacidade) na aba Controls do menu do jogo; o botão voltar do Android abre o
  menu;
- controles Bluetooth (testado com um GameSir X4 Aileron), com botões remapeáveis na aba Controls;
- uma opção de taxa de quadros (aba Video): Auto (a escolha de 30 / 60 do próprio jogo), 30 fps ou 60 fps;
- um overlay de desempenho (fps, tempo de quadro, frequência e uso de CPU, diagnóstico do ADPF e núcleos por thread, GPU, memória, temperatura, bateria), e sem tela preta quando o app
  volta do segundo plano. O menu do jogo deixa de fora as opções do desktop (tela cheia, 4K / 8K, randomizer).

### O que deixou rápido

Os shaders do jogo são traduzidos do código da GPU do Switch para GLSL em tempo de execução. No Adreno os shaders
traduzidos eram o gargalo, então no Android o tradutor agora:

- resolve as pilhas de controle de fluxo na tradução, em vez de mantê-las como arrays em cada shader;
- transforma desvios, laços e tabelas de salto (lidas das constantes do próprio shader) em código estruturado
  `if` / `for` / `switch`, em vez de uma máquina de estados `switch (pc)`, que o compilador mobile lidava mal (o
  shader de iluminação do mapa aberto foi de 24 para 16-19 registradores e de 2884 para 1717 instruções);
- converte valores de meia precisão com as instruções da própria GPU nos shaders de fragmento e de compute.

No lado da CPU:

- **uma thread de renderização**: as listas de comandos do jogo são traduzidas para Vulkan numa thread própria
  enquanto o jogo trabalha no quadro seguinte, e entregues à GPU em partes, para ela começar antes de o quadro inteiro
  estar traduzido. A thread do jogo passou de ~11,4 para ~7,4 ms de trabalho por quadro;
- **o núcleo 0 do Switch, emulado**: o jogo roda algumas das suas threads de tarefas no mesmo núcleo da thread
  principal, onde elas se revezam; em paralelo de verdade, uma thread principal mais rápida expunha uma condição de
  corrida no sistema de tarefas (um crash). Agora essas threads se revezam aqui também, e os outros núcleos do jogo
  seguem em paralelo;
- **controle de ritmo de quadros** que distingue os quadros de 30 fps do próprio jogo de um quadro de 60 fps que
  atrasou, dicas de desempenho do Android (ADPF) com sessão unificada para as duas threads mirando 60 fps, e correções de três condições de
  corrida de sincronização no kernel emulado;
- o runtime é compilado com otimização, os fences de GPU do jogo não esperam mais o compositor, e o trabalho por draw
  é guardado em cache.

## Turnip ou o driver do próprio celular

O jogo roda, **com a iluminação**, tanto com o driver **Turnip** do Mesa quanto com o driver **Qualcomm** do próprio
celular. O Turnip continua sendo o indicado para **60 fps**:

| Driver | Iluminação | Mapa aberto (720p) |
|---|---|---|
| Turnip | sim | 60 fps (98% do tempo) |
| Qualcomm do celular | sim | 30 fps |

![Vila Mabe no driver Qualcomm do próprio celular, com a iluminação, 30 fps](docs/android/qualcomm_driver.jpg)

No driver Qualcomm do próprio celular, sem o Turnip (a linha da GPU no overlay diz "Qualcomm"): a iluminação está lá.

Os shaders do jogo leem e escrevem diretamente na memória do jogo, nos mesmos endereços que a CPU usa: por exemplo, a
lista de luzes de cada quadro é montada por um compute shader e depois lida pelos shaders de pixel, e alguns shaders
chegam aos dados por ponteiros. No PC, a memória do jogo é importada para a GPU com `VK_EXT_external_memory_host`,
que nenhum dos dois drivers tem.

- O **Turnip** tem `VK_EXT_map_memory_placed`: a memória da GPU é alocada exatamente no endereço do heap do jogo,
  então CPU e GPU compartilham a mesma memória, sem cópias.
- **Qualquer outro driver** (o da Qualcomm e, em princípio, Mali, Xclipse e outros) passa por uma **arena na GPU**:
  as regiões de memória que os shaders escrevem ficam na GPU, então os draws que as leem veem os resultados no mesmo
  quadro; as mudanças da CPU são detectadas e copiadas pela GPU, e os resultados da GPU voltam para a memória do jogo
  quando ela termina. As regiões que os shaders alcançam por ponteiros são descobertas durante o jogo: os shaders
  registram as leituras fora das regiões conhecidas e elas são adicionadas. Isso custa mais que a memória
  compartilhada do Turnip, daí os 30 fps por enquanto.

## Limitações atuais

- Testado em Snapdragon (Adreno): com o Turnip a 60 fps, com o driver do próprio celular a 30 fps (com a iluminação
  nos dois). O app carrega o Turnip com [libadrenotools](https://github.com/bylaws/libadrenotools); o driver não está
  incluído, você fornece o seu. **Mali**: o jogo rodou em uma Mali-G615 a 20-30 fps; desde a 0.3.3 as texturas são
  decodificadas na CPU (a Mali não tem texturas BC, o formato do Switch), o que corrige os blocos coloridos vistos
  nela - ainda a confirmar em mais celulares Mali. Xclipse e outras devem funcionar pelo mesmo caminho: relatos são
  bem-vindos.
- Você precisa do seu próprio dump v1.0.0 e de cerca de **12 GB livres** para a configuração (o app guarda a sua
  própria cópia dos arquivos do jogo mais uma imagem de dados gerada a partir deles); o dump pode ser apagado depois.
- A primeira configuração compila o código do jogo no celular: cerca de 20 minutos em um Snapdragon 8 Elite.
- Mudar a resolução com o jogo rodando pode fechar o app. Mude na tela de título.
- Ainda sem randomizer no celular.

## Instalação

Você precisa de um celular arm64 com Android 11 ou mais novo (testado num Snapdragon), dos seus próprios arquivos
extraídos da v1.0.0 (as pastas `exefs` e `romfs`) copiados em alguma pasta do celular e, para 60 fps numa GPU Adreno,
de um pacote do driver Turnip (o `.zip` do adrenotools, o mesmo usado por outros emuladores).

1. Instale o APK da página de [Releases](https://github.com/mamcjr/LASRecompAndroid/releases).
2. Abra o app, toque em **Game files folder** e escolha a pasta que contém `exefs` e `romfs` (ou a pasta acima dela).
   O Android pede permissão de acesso a ela: o app copia os arquivos para a sua própria pasta.
3. Aguarde a configuração: a cópia, a imagem de dados do jogo e o código do jogo (cerca de 20 minutos na primeira
   vez; mantenha a tela ligada).
4. Opcional, para 60 fps em Adreno: **Settings > Prelaunch > Vulkan driver > Choose driver**, escolha o `.zip` do
   Turnip e depois **Restart now**. Sem isso o jogo roda no driver do próprio celular, com a iluminação, a 30 fps.
   Se um driver deixar a tela embaralhada ou o app não abrir, segure o ícone do app e escolha
   **Driver do sistema**: o app volta para o driver do próprio celular. Testados no Adreno 830 (Snapdragon 8 Elite):

   | Driver | Resultado |
   | --- | --- |
   | Turnip Gen8 V37 | funciona, 60 fps |
   | [whitebelyash](https://github.com/whitebelyash/freedreno_turnip-CI/releases) Stable Turnip V2, Mainline Turnip V32, A8XX Turnip V29 | funcionam, 60 fps |
   | Turnip v26.3.0-R6 | tela embaralhada, depois fecha no jogo |
   | [K11MCH1](https://github.com/K11MCH1/AdrenoToolsDrivers/releases) Turnip A8XX Draft (`turnip_a8xx.zip`) | a GPU para de responder (device lost) |
   | K11MCH1 Turnip v26.0.0 R8 | feito para A7xx: sem GPU aqui, então o app usa o driver do próprio celular |
5. **Play**, com os controles de toque ou um controle. O botão de menu na tela, o botão voltar ou Menos + Mais no
   controle abrem o menu do jogo.

O app guarda tudo em `Android/data/com.lasrecomp.android/files/`: saves, configurações, o cache de pipelines e os
arquivos do jogo. Desinstalar o app apaga essa pasta, inclusive os saves: copie a pasta `save` para fora antes.

## Compilando a partir do código

No Windows com Git Bash, o Android SDK com **NDK 30.0.16248370** e **CMake 3.30.4**, um celular com depuração USB e
Python 3.12. No Linux os mesmos passos devem funcionar com bash, mas não foram testados.

1. As dependências do app (SDL3, shaderc, Vulkan-Headers, VMA, volk e a fonte DejaVu):
   `bash android/deps/build_deps.sh`.
2. Opcional: FFmpeg, para os vídeos e o fundo da tela inicial. O `configure` dele exige um shell Unix, então no
   Windows rode no WSL: `wsl -- bash android/deps/build_ffmpeg.sh`.
3. O compilador do código do jogo que o app carrega (Python 3.14 para Android, capstone, lz4, zig):
   `bash android/deps/build_gen_deps.sh`; o unicorn também exige um shell Unix:
   `wsl -- bash android/deps/build_unicorn.sh`.
4. O app: `cd android/lasrecomp && ./gradlew assembleRelease` (assinado com a sua chave de
   `~/.lasrecomp/keystore.properties` se ela existir, senão com a chave de debug), depois
   `adb install -r app/build/outputs/apk/release/app-release.apk`, e configure como em [Instalação](#instalação).

Para desenvolvimento, o código do jogo ainda pode ser compilado no PC e enviado:
`bash android/build_gamelib.sh <dump>/exefs` (com `python -m venv tools/venv` e `pip install lz4 capstone unicorn`),
depois `bash android/push_game.sh <pasta com exefs/ e romfs/>`.

## Créditos

- **[LAS Recomp](https://github.com/peiikai/LASRecomp)**, de peiikai: o recompilador, o runtime, o renderizador Vulkan
  e o tradutor de shaders sobre os quais este port foi feito. O README dele dá os créditos a todos os envolvidos,
  incluindo o LAS-Randomizer e os mods da comunidade que as opções dele reimplementam.
- [SDL 3](https://libsdl.org) (zlib): janela, entrada, áudio e a activity do Android.
- [libadrenotools](https://github.com/bylaws/libadrenotools), de bylaws (BSD 2-Clause): carrega um driver Vulkan
  personalizado.
- [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html) (MIT): o driver Vulkan para o qual o app foi feito
  (não incluído).
- [shaderc](https://github.com/google/shaderc) (Apache 2.0), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
  (Apache 2.0 / MIT), [volk](https://github.com/zeux/volk) (MIT) e
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (MIT).
- [FFmpeg](https://ffmpeg.org) (LGPL 2.1+, opcional, ligado dinamicamente): reprodução dos vídeos.
- O compilador do código do jogo, executado no celular: [Python](https://www.python.org) 3.14 (licença PSF),
  [Zig](https://ziglang.org) 0.17 (MIT), [Capstone](https://github.com/capstone-engine/capstone) 5.0.9 (BSD 3-Clause),
  [Unicorn](https://github.com/unicorn-engine/unicorn) 2.1.4 (GPL 2.0; o código-fonte sem modificações:
  [2.1.4](https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz), compilado por
  `android/deps/build_unicorn.sh`) e [LZ4](https://github.com/lz4/lz4) 1.10.0 (BSD 2-Clause).
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT): o menu sobreposto e a tela inicial.
- [SMAA](https://github.com/iryoku/smaa) e [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  (MIT): as opções de anti-aliasing e de upscaler, embutidas no renderizador.
- O robô do Android na tela inicial é reproduzido ou modificado a partir de trabalho criado e compartilhado pelo
  Google e usado de acordo com os termos da [Licença Creative Commons Atribuição 3.0](https://creativecommons.org/licenses/by/3.0/).
- [Fontes DejaVu](https://dejavu-fonts.github.io) (licença Bitstream Vera): substituta da fonte de sistema do Switch.

## Aviso legal

Este é um projeto de fã. Não tem afiliação, endosso nem patrocínio da Nintendo. *The Legend of Zelda* e *Link's
Awakening* são marcas registradas da Nintendo. Nenhum arquivo, código, recurso ou fonte do jogo está incluído, neste
repositório ou em qualquer build. Você precisa fornecer a sua própria cópia, obtida legalmente, dos arquivos do jogo
para jogar.

## Contribuições e suporte

Relate problemas e sugestões do Android nas [Issues deste fork](https://github.com/mamcjr/LASRecompAndroid/issues).
Português e inglês são bem-vindos. Consulte [CONTRIBUTING.md](CONTRIBUTING.md) para saber quais informações do
aparelho incluir, como desenvolver para Android e como validar e enviar alterações.

## Licença

O código é aberto sob a [Licença MIT](LICENSE). O aviso original de copyright de Peiikai foi preservado;
um aviso adicional identifica as contribuições de mamcjr e dos colaboradores do LASRecompAndroid.
Os componentes de terceiros mantêm as suas próprias licenças (acima, e no
[README original](https://github.com/peiikai/LASRecomp#license)). A licença cobre
apenas o código deste projeto: ela não concede nada sobre o jogo da Nintendo, que você mesmo precisa fornecer.
