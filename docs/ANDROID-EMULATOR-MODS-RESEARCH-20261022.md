# Mods de desempenho e updates de Link's Awakening

Pesquisa em **22/10/2026**, comparada ao código local no commit `80af5aa`. Escopo: pesquisa na internet, leitura dos patches públicos e inspeção estática do port. **Nenhum teste, instalação ou comando no celular; nenhum código de execução ou configuração alterado.** Não há novos números de desempenho nesta pesquisa. ADPF permanece como está.

## Conclusão para o projeto

Há ideias aproveitáveis, principalmente retirar trabalho real do pós-processamento e investigar retenção de buffers/esperas de apresentação. Os mods históricos de 60 FPS e blur já têm equivalentes conceituais no port; instalar os mesmos patches não implica uma otimização adicional. Para a prioridade atual, conservar o modo nativo de 30 FPS e corrigir compatibilidade vem antes de forçar 60/120.

Não há evidência pública suficiente para recomendar o update como otimização Android. O **1.0.1** interessa por uma correção de progressão; o **1.1.0**, por melhorias de imagem para Switch 2. Suporte a eles requer adaptação da recompilação e dos hooks, não apenas copiar o update para a instalação.

## Mods encontrados: efeito e possibilidade de aproveitamento

| Mod / técnica | Evidência consultada | Utilidade neste port |
| --- | --- | --- |
| Stable 60 FPS / Stable-60fps-v2 | Patch textual arquivado: dois NOPs impedem a troca dinâmica para 30; alvo 1.0.0 | O modo 60 já intervém na escolha de cadência. Pode evitar uma queda automática quando há margem, mas não torna os desenhos mais baratos; manter 30 nos aparelhos que não sustentam 60 |
| 60FPS stable V3 / variante 1.1.0 | Catálogo do jogo e página do mod 1.1.0 confirmam sua existência; payload destas variantes não foi obtido | Não atribuir às variantes a mesma implementação da v2 nem prometer ganho. Comparar seus patches completos, Build IDs e instruções antes de considerar uma adaptação |
| Blur Removal / No DOF / Disable TiltShift | O arquivo histórico altera o nome de um parâmetro; o projeto também contém uma implementação derivada desse conceito | Útil para o relato de blur superior/inferior. A questão de desempenho é saber se os passes continuam executando depois de o efeito ficar invisível |
| Fix Bloom Effects do Eden | Commit público modifica amostras de textura e condiciona um caminho de conversão de profundidade | Referência de compatibilidade visual; não tratar como remoção seletiva de passes nem ganho de FPS |
| Yuzu Softlock Fix | Consta no catálogo de mods; implementação não inspecionada | Investigar apenas com um bloqueio reproduzível correspondente. Não confundir com a correção oficial da Marin ou com otimização |
| Ultrawide and Extended Culling | Consta no catálogo do jogo | Recurso visual, sem evidência de ganho. Ampliar a área visível pode aumentar carga; 16:9 é um controle útil |
| Redução de sombras, LOD, grama ou reflexos | Não foi confirmado um patch público específico, inspecionável, de desempenho para este jogo nesta pesquisa | Possibilidades próprias do renderer, dependentes de perfil por passe; não apresentar como mods já comprovados nos emuladores |

Referências: [catálogo arquivado do yuzu](https://github.com/yuzu-mirror/yuzu-mod-archive#the-legend-of-zelda-links-awakening), [catálogo de autores no GameBanana](https://gamebanana.com/mods/games/7562), [variante 60fps Stable 1.1.0](https://gamebanana.com/mods/691512), [No DOF Blur](https://gamebanana.com/mods/14847). O nome de um mod e comentários de usuários não demonstram seu custo ou sua compatibilidade neste port.

### O que foi efetivamente lido nos patches históricos

O [Stable-60fps-v2](https://github.com/yuzu-mirror/yuzu-mod-archive/blob/main/files/The%20Legend%20of%20Zelda%20Links%20Awakening/Stable-60fps-v2.zip) declara Build ID `AE16F71E002AF8CB059A9A74C4D90F34BA984892` e `offset_shift 0x100`. Seus dois registros, `01019608` e `01019628`, recebem `1F2003D5`, bytes de um NOP AArch64. Isso retira decisões de cadência; não remove geometria, shaders ou pós-processamento. Esses são offsets do formato do patch, não endereços prontos para copiar para um hook.

O [Blur_Removal](https://github.com/yuzu-mirror/yuzu-mod-archive/blob/main/files/The%20Legend%20of%20Zelda%20Links%20Awakening/Blur_Removal.zip) inclui arquivos para 1.0.0 e 1.0.1, com Build IDs distintos. Ambos substituem texto no registro `016CBD76`. Não são um otimizador geral dos shaders. Não se verificou aqui equivalência entre esse pacote e todas as variantes de No DOF/TiltShift.

Os arquivos foram apenas lidos como texto dentro dos ZIPs. SHA-256 das cópias consultadas:

| Arquivo | SHA-256 |
| --- | --- |
| Stable-60fps-v2.zip | `13085853f34ef5be6afb27e40a221f2c3311aa85a88aa28697e2894db02297bc` |
| Blur_Removal.zip | `8d1840d4681bc6c91f4f559c48d8000550584675bdb750a0bf1291a15c4b8023` |

### Comparação com o código já existente

- [qol.c](../runtime/hle/qol.c), `rt_tilt_obj` / `apply_tilt`: troca a primeira letra de `PFXTiltShiftParam` na cópia usada pelo objeto. [cosmetics.asm](../third_party/LAS-Randomizer/RandomizerCore/ASM/Patches/cosmetics.asm) contém a ideia de renomear o parâmetro, com crédito a theboy181. O relato do mantenedor sobre blur nas bordas continua sendo uma questão de correção; esta pesquisa não a declara resolvida.
- [exec.c](../runtime/nvn/vulkan/exec.c), `pace_end_counter` e seleção Auto/30/60: o port já influencia a cadência percebida pelo jogo. [framepace.c](../runtime/hle/framepace.c) preserva o modo nativo de 30. Outro patch de 60 pode conflitar com esse controle e não estabelece 120 FPS com velocidade correta.
- [output_scale.c](../runtime/nvn/output_scale.c): resolução fixa, frações da tela e Dynamic já existem. Os percentuais são relativos a Match Display; 1x usa a referência de 1080 linhas. Comparações precisam registrar os pixels efetivos.
- [async.c](../runtime/nvn/vulkan/async.c), [exec.c](../runtime/nvn/vulkan/exec.c) e [device.c](../runtime/nvn/vulkan/device.c): já há thread de tradução/renderização, reaproveitamento de pipelines e cache Vulkan. “Ativar async/cache” de um guia de emulador não é uma funcionalidade inteiramente ausente aqui.

**Candidato concreto identificado por inspeção:** `dyn_update` ainda usa limiares de aproximadamente 60 FPS (`17,4 ms` por apresentação), enquanto o caminho Vulkan chama `output_scale` sem tempo GPU (`-1`). Em 30 FPS fixos, o intervalo esperado de aproximadamente 33,3 ms satisfaz o critério de lentidão, mesmo quando o jogo cumpre a meta. Isso pode reduzir a resolução desnecessariamente. Investigar alvo de Dynamic coerente com Auto/30/60 e tempo de trabalho separado da espera. É uma observação do código, não uma medição de defeito ou ganho no aparelho. Não mudar ADPF para resolver isso.

## Comparação entre emuladores

| Projeto | O que a pesquisa permitiu verificar | Consequência para LASRecompAndroid |
| --- | --- | --- |
| yuzu histórico | Arquivo de mods documenta Stable 60 e Blur Removal, com versões específicas | Aproveitar a intenção dos patches, respeitando a compilação estática; não aplicar seus offsets cegamente |
| Eden | Manual separa mods de RomFS/ExeFS de frameworks; commits explicam workaround de bloom e correção de apresentação específica que menciona este jogo | Melhor referência técnica nova encontrada; avaliar os mecanismos separadamente |
| Skyline, emulador Android | Repositório oficial informa desenvolvimento encerrado e retirada do código das branches/tags controladas pelos responsáveis | Não foi possível confirmar nesse material uma otimização específica de Link's Awakening para transplantar |
| Egg NS | Site do projeto apresenta produto e requisitos; não forneceu nesta pesquisa patch textual ou explicação técnica verificável para este jogo | Não há base para atribuir ao Egg NS uma técnica específica reaproveitável ou comparar FPS |
| Ryujinx e derivados | Os mods estudados alteram o jogo, mas esta pesquisa não confirmou o carregamento de cada variante em cada derivado | Compatibilidade de formato/mod por versão continua sendo uma checagem separada; não há benchmark comparativo aqui |

Fontes: [manual de mods do Eden](https://github.com/eden-emulator/mirror/blob/master/docs/user/Mods.md), [registro oficial do Skyline](https://github.com/skyline-emu/skyline), [site do Egg NS](https://eggns.wordpress.com/). **Skyline, o emulador Android, é diferente do framework Skyline de plugins de jogos** citado no manual do Eden. Não usar a documentação de plugins como evidência sobre o emulador.

NCE/JIT, precisão de GPU, modo portátil/dock, ASTC e cache de shaders são configurações de emulador, não mods equivalentes. O port recompila a CPU e implementa NVN/Vulkan por outro caminho. Não há um botão NCE/JIT a reproduzir; cache binário de outro renderer/driver também não pode ser presumido compatível.

### Eden: apresentação e retenção de buffers

O [commit `989768aa46720e3fcab9e2111e3ddd70819c382b`](https://github.com/eden-emulator/mirror/commit/989768aa46720e3fcab9e2111e3ddd70819c382b), de 19/10/2026, menciona corrigir regressão de desempenho em Link's Awakening. O diff está em `hardware_composer.cpp`: evita aquisição quando nenhuma camada é visível e move a devolução de framebuffers para depois da entrega ao compositor, no mesmo período, evitando reter uma posição da fila por um frame extra. O próprio patch registra a pendência de release fences e risco de tearing.

**Inferência para o port:** auditar retenção de imagens, limite de apresentações pendentes, `vk_wait_seq`, acquire e bloqueios da thread de renderização. O port não usa esse compositor HLE; copiar o diff não resolve automaticamente seu pacing. Não devolver imagens ainda em uso nem remover fences. Essa hipótese se conecta à investigação já existente de contadores GPU e esperas de apresentação.

### Eden: workaround de bloom não equivale a otimização

O [commit `4a5bec1cfc7f2a4abfe2fc384acd9ccd0cf79aac`](https://github.com/eden-emulator/mirror/commit/4a5bec1cfc7f2a4abfe2fc384acd9ccd0cf79aac), de 26/01/2026, adiciona a opção `fix_bloom_effects`. No caminho Android de amostragem com LOD explícito, multiplica o vetor amostrado por **0,98** quando habilitada; também condiciona uma conversão de profundidade `D32_FLOAT` à opção. A [descrição do PR](https://git.eden-emu.dev/eden-emu/eden/pulls/3359) informa remoção parcial do blur em LA/EOW em Adreno A7xx e anteriores.

Isso altera valores da imagem e pode atingir materiais além do efeito desejado. Não elimina seletivamente o trabalho do tilt-shift. Não há, nessa evidência, prova de ganho de FPS ou correção completa. Serve para investigar precisão/amostragem e conversões, preservando iluminação, água, grama e textos como controles visuais. Não adotar o fator global 0,98 como solução padrão do port.

## Updates oficiais: o que pode beneficiar o port

A [Nintendo documenta duas atualizações](https://www.nintendo.com/en-gb/Support/Nintendo-Switch/Game-Updates/How-to-Update-The-Legend-of-Zelda-Link-s-Awakening-1666739.html):

| Versão | Mudança publicada | Avaliação para este projeto |
| --- | --- | --- |
| 1.0.1 — 25/10/2019 | Corrige Marin parada em Toronbo Shores, que bloqueava o jogo em certas condições; também corrige saves afetados | Benefício de correção/progressão. As notas não anunciam otimização de desempenho |
| 1.1.0 — 04/06/2025 | Para Switch 2: melhorias de imagem para sua tela/TVs de alta resolução e suporte a HDR | Não demonstra redução de custo em Android. Qualidade/resolução maior pode aumentar trabalho se esse caminho for ativado; efeito real depende do código e do hardware |

O desempenho observado no Switch 2 não isola software de hardware. As notas não prometem 120 FPS e não comprovam aceleração no Switch original ou neste port. Possíveis otimizações internas não descritas permanecem desconhecidas sem comparação de executáveis/dados.

### Por que o update não pode ser simplesmente carregado

O [launcher](../runtime/ui/launcher.mm), `build_id_ok` / `nativeBuildIdOk`, confere oito bytes do Build ID do `main` contra o alvo conhecido. O README exige **1.0.0**. [patches.py](../tools/patches.py) contém endereços fixos do executável e [gen.py](../tools/gen.py) gera C a partir das instruções da versão escolhida. O pacote histórico de blur já mostra IDs diferentes entre 1.0.0 e 1.0.1. Não basta remover a checagem de versão.

Um suporte futuro precisaria:

1. Comparar dumps próprios completos e coerentes de ExeFS/RomFS de cada versão; registrar Build IDs completos e hashes, sem distribuir arquivos do jogo.
2. Identificar funções, dados e recursos modificados, em vez de supor que offsets coincidentes de um único mod provam equivalência geral.
3. Remapear hooks e estruturas, regenerar a recompilação, ajustar seleção de versão e invalidar artefatos gerados incompatíveis.
4. Verificar HLE/serviços solicitados, renderização, integridade dos saves e progressão; depois comparar desempenho sob a mesma qualidade e velocidade.

Não foi obtido nem analisado um dump do update nesta pesquisa. Logo, não se sabe quais funções mudaram, se o bloqueio de Marin já está evitado por algum comportamento atual, ou quanto custaria adaptar cada versão. Uma correção localizada equivalente à 1.0.1 pode ser uma alternativa, mas exige localizar a causa e validar saves afetados; não presumir que copiar dados atualizados basta.

## Ordem recomendada para trabalho posterior

| Prioridade | Frente | Resultado que justificaria adotá-la |
| --- | --- | --- |
| 1 | Compatibilidade e 30 FPS fixos | Inicialização e imagem correta em aparelhos representativos, incluindo Mali; preservar ADPF e o passo nativo de 30 |
| 2 | Blur correto e eventual retirada de passes | Desfoque superior/inferior realmente removido; se for otimização, comprovar menos trabalho GPU/passes, e não só alteração visual do parâmetro |
| 3 | Dynamic coerente com meta de 30/60 | Não reduzir qualidade por interpretar a espera normal de 30 como sobrecarga; comparar pixels e custo sob a mesma meta |
| 4 | Retenção de buffers/apresentação | Menos espera desnecessária e p95/p99 melhores, sem tearing, uso prematuro de imagem ou maior fila de latência |
| 5 | Pipelines e texturas | Medir misses de compilação, decode e uploads antes de ampliar os caches/reuso já presentes; distinguir primeira visita de cache aquecido |
| 6 | 1.0.1 ou correção equivalente | Resolver o caso de progressão e preservar saves; avaliar desempenho separadamente |
| 7 | 1.1.0 e 120 FPS | Só após a base de compatibilidade; exigir benefício demonstrado, qualidade controlada e velocidade correta de gameplay |

Este roteiro não inicia testes. Quando autorizados novamente, usar os mesmos saves/cenas, resolução efetiva, driver, temperatura e qualidade; fazer comparações repetidas com frametime, tempo CPU/GPU e imagem. Um aumento de FPS por sumir iluminação, grama ou textos é regressão visual, não otimização. A [matriz S23 já registrada](ANDROID-STATIONARY-MATRIX.md) permanece uma referência anterior, sem novos resultados anexados por esta pesquisa.

## Limites e rastreabilidade

- Foram inspecionados os payloads históricos v2/Blur Removal e os dois commits do Eden, somente como dados/código-fonte. ZIPs e respostas da API ficaram em `build/mod-research-20261022/`, fora da documentação versionada; não foram executados.
- As páginas GameBanana dependem de JavaScript e a API retornou 403. A existência de V3/1.1.0 está documentada no catálogo/página, mas seus bytes e mudanças não foram auditados. O Softlock Fix e Extended Culling também não tiveram payload inspecionado.
- O material do Egg NS não permitiu atribuição técnica específica; o repositório atual do Skyline não disponibiliza seu renderer. Ausência de evidência obtida não significa ausência de otimizações nesses projetos.
- Não há evidência suficiente para prometer porcentagem de ganho, selecionar um driver universal ou garantir 30 FPS para toda GPU Mali. Adaptações futuras devem conservar créditos e verificar a licença do material que vier a ser incorporado; nenhum código desses projetos foi transplantado nesta pesquisa.
