# Metas de desempenho Android

Pesquisa de referência: [mods de desempenho nos emuladores e updates 1.0.1/1.1.0](ANDROID-EMULATOR-MODS-RESEARCH-20261022.md), de 22/10/2026. Inclui comparação com o código do port e candidatos para investigação; somente documentação, sem testes novos ou ganhos anunciados.

Direção definida pelo mantenedor em 21/10/2026:

**Prioridade revisada:** primeiro inicializar sem erros nos modelos suportados e comprovar 30 FPS estáveis em vários dispositivos. O desenvolvimento do modo 120 fica adiado até essa base estar validada. As etapas de 120 abaixo são um roteiro posterior, não a frente ativa.

| Perfil | Meta | Orçamento de apresentação |
| --- | --- | ---: |
| Snapdragon 8 Elite + Turnip | 120 FPS, velocidade correta da gameplay | 8,33 ms |
| Snapdragon 8 Elite Gen 5 + Turnip | 120 FPS, validação própria nesse hardware | 8,33 ms |
| Aparelhos mais fracos, incluindo GPUs Mali | Pelo menos 30 FPS estáveis nos modelos suportados | 33,33 ms a 30 FPS |

São objetivos de desenvolvimento, não resultados já atingidos ou garantias por família de GPU. O ponto inicial de comparação conserva 50%/FSR1/AA desligado usados nas medições atuais; registrar a resolução interna e a saída em pixels em cada validação. A qualidade para cada perfil precisa ser explicitada antes de comparar aparelhos. O perfil de 30 FPS prioriza cadência estável e velocidade correta, sem exigir as mesmas opções gráficas do perfil de 120.

## Bloqueio atual para 120 FPS

O código atual oferece Auto (30/60), 30 e 60 em `runtime/settings.h`, `runtime/settings.c` e `runtime/ui/menu.mm`. O VSync emulado de `runtime/hle/vi.c` tem período fixo de 16.666.667 ns. O pacer Vulkan avalia o orçamento de 60 FPS e alterna para o modo de 30 do jogo quando necessário. Não há um modo de 120 FPS implementado nesses caminhos.

As observações registradas no pacer descrevem um passo de mundo por frame no modo 60 e dois no modo 30. A experiência antiga de `runtime/hle/framepace.c` não resolveu o tempo da gameplay: o próprio arquivo registra que forçar aquele contador não alterou a velocidade de caminhada. Portanto, mudar só o VSync, o limitador ou esse contador não estabelece 120 FPS corretos. Não adicionar uma opção de menu que acelere o jogo ou apenas repita imagens e rotulá-la como 120 FPS.

### Ordem de investigação para o perfil de 120

1. Identificar o avanço real de simulação/animações e sua relação com a geração de comandos de renderização. Medir caminhada, duração de animações, física, diálogos e áudio em relação ao tempo real como controles de velocidade.
2. Criar um caminho experimental separado para renderizar a 120 preservando a velocidade correta, seja com timestep apropriado ou separação/interpolação de renderização e simulação após comprovar a arquitetura necessária. Não presumir que isso já existe ou que simples replay de comandos é correto.
3. Conferir apresentação e taxa efetiva da tela. Medir timestamps e conteúdo apresentado: ter uma tela a 120 Hz não prova que o jogo gerou 120 imagens novas por segundo.
4. Só então medir orçamento CPU/GPU de 8,33 ms e atacar os passes dominantes: composição/upscaling, submissões, cópias e tradução de comandos, conforme evidência. Ganhos em uma execução limitada a 60 não demonstram capacidade de 120.

O comportamento estável do ADPF permanece preservado nesta fase: sinal de tempo de parede, margem e sessões atuais, sem CPU-only, afinidade ou hints GPU experimentais. A adequação do alvo ao futuro modo 120 fica como uma decisão explícita posterior; esta definição de metas não altera o alvo atual nem autoriza tratar 120 como implementado.

## Frente de 30 FPS e compatibilidade

Primeiro separar falhas de inicialização/importação/driver dos aparelhos que abrem com desempenho baixo. Os comentários disponíveis não informam versão, driver ou erro suficiente para identificar uma causa comum. Não bloquear nem certificar uma família inteira de GPUs pelos nomes dos chips.

Nos aparelhos que abrem, medir o modo fixo de 30 com pacing uniforme e velocidade correta; evitar usar oscilações Auto 30/60 como referência de estabilidade. Obter dados reais de CPU decode BC/ASTC, staging, picos de compilação, memória, apresentação e aquecimento. Priorizar o custo que aparecer nessas medições e validar visualmente os fallbacks de formatos/recursos.

Forçar BC pela CPU no Adreno exercita um caminho de software, mas não reproduz desempenho, funcionalidades ou erros do driver Mali. A meta precisa ser comprovada em aparelhos representativos, com suas configurações e limites de Vulkan registrados. Não há garantia de 30 FPS em toda GPU que recebe o nome Mali.

## Protocolo de validação

A rota v1 detectou deriva em algumas repetições e uma troca de superfície de coleta. Antes de usá-la como evidência fina de 30 contra 120, substituir o controle exclusivamente por frames por um protocolo de pontos de passagem/tempo de simulação que preserve o percurso entre modos. Não aumentar a tolerância depois de ver os resultados. Aquecer o percurso inteiro, distinguir criação de pipelines de voltas já aquecidas, e medir também outras cenas representativas e uma sessão prolongada.

O coletor aceita `--target-fps 30|60|120` **somente para análise**: não seleciona modo do jogo, não solicita taxa da tela e não modifica ADPF. Além de p50/p95/p99, informa o orçamento e a porcentagem de intervalos acima de `budget + max(0,5 ms, 5% do budget)`. Isso corresponde a aproximadamente 35 ms para 30 FPS e 8,83 ms para 120 FPS. A tolerância torna explícita a diferença entre ruído de timestamp/cadência da tela e um atraso relevante; ela não aumenta o orçamento de renderização de 33,33/8,33 ms.

Uma rodada de 60 FPS medida com orçamento analítico de 120 deve falhar na meta de 120; mudar o argumento não é uma otimização. `over20_pct`, mantido para os relatórios anteriores, não é o indicador adequado para o perfil de 30 nem para o de 120. Exigir também qualidade/velocidade equivalentes, controle de temperaturas e comparação repetida; FPS médio isolado não certifica a meta.

As [comparações anteriores](ANDROID-VILLAGE-RESULTS-20261021.md) são referências no caminho atual de 60 FPS. Elas não validam 120 ou desempenho real em Mali. Nenhuma publicação de ganho deve ocorrer antes de comprovação de frametime e correção da gameplay no perfil anunciado.
