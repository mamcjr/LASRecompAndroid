# Compatibilidade Android — relatos da comunidade

Metas de desenvolvimento: [120 FPS com Turnip no 8 Elite/8 Elite Gen 5 e pelo menos 30 FPS estáveis nos aparelhos mais fracos suportados](ANDROID-PERFORMANCE-TARGETS.md). Não confundir essas metas com os resultados preliminares abaixo.

Registro inicial em 21/10/2026, a partir de comentários no YouTube repassados pelo mantenedor. São relatos preliminares, sem reprodução controlada. A versão do APK, Android, driver, cena, duração e configurações não foram informadas, exceto onde indicado. Não representam uma certificação de compatibilidade nem permitem calcular uma taxa de sucesso.

| Aparelho ou chip informado | Resultado relatado | Resolução informada | Próxima investigação |
| --- | --- | --- | --- |
| Motorola Signature | Rodou perfeitamente; FPS não informado | Não informada | Obter configurações e duração para referência |
| MediaTek Dimensity 7300 | 30 FPS, com quedas para 18–22 FPS | Não informada | Separar custo de renderização, modo de FPS e aquecimento |
| MediaTek Dimensity 7300 Ultimate | 30 FPS, com quedas para 18–22 FPS | Não informada | Mesmo protocolo do Dimensity 7300 |
| Helio G100 | Funcionou; FPS não informado | Não informada | Confirmar cena, estabilidade e configurações |
| Snapdragon 8 Gen 1 | Não funcionou | Não informada | Identificar etapa da falha e mensagem de erro |
| POCO F5 | Tela preta | Não informada | Identificar etapa da falha e mensagem de erro |
| MediaTek Helio G99 Ultra | Tela preta / erro | Não informada | Identificar etapa da falha e mensagem de erro |
| Samsung A15 | Tela preta / erro; variante não informada | Não informada | Confirmar variante e etapa da falha |
| Snapdragon 8s Gen 4 | 54 FPS em 50%; 40 FPS em 75% | 50% e 75% | Comparar a mesma cena e medir frametime |

Os nomes acima preservam os relatos. Não inferir o chip de um modelo, o driver usado ou uma falha comum a toda uma família de GPUs. “Funcionou” não significa 60 FPS estáveis. A diferença entre 50% e 75% no 8s Gen 4 justifica investigar o custo da renderização, mas sozinha não identifica um gargalo exclusivamente na GPU.

## Prioridades

A frente ativa é compatibilidade de inicialização e 30 FPS estáveis em vários aparelhos suportados. O modo 120 fica adiado. Novas instalações Android partem de 30 FPS fixos e 50% de resolução interna; configurações existentes carregadas do arquivo continuam tendo precedência. Isso é um padrão de menor carga, não comprovação de desempenho em uma GPU.

### Revisão de inicialização — 21/10/2026

A criação da swapchain deixou de exigir transferência para destino, uso não empregado pelo código de apresentação. Transferência para origem só é solicitada quando suportada; sem ela, a captura interna da janela é desabilitada, mantendo a renderização. O modo de composição é escolhido entre os bits suportados, em vez de sempre solicitar opaco. Listas de formatos vazias/erro passam a gerar um diagnóstico explícito; a consulta de imagens verifica resultados e o limite interno de oito imagens antes de usá-las. Isso segue as [capacidades da superfície](https://docs.vulkan.org/refpages/latest/refpages/source/VkSurfaceCapabilitiesKHR.html) e os [requisitos de criação da swapchain](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html).

O cache nativo de pipelines agora limita a leitura a 64 MiB, trata falhas de leitura/alocação e confere o cabeçalho público de 32 bytes, versão, fabricante, dispositivo e UUID. Arquivos rejeitados são ignorados e o cache começa vazio; o arquivo antigo não é apagado na leitura. Um erro do driver ao criar o cache recebe nova tentativa sem dados. Não é checksum nem validação do conteúdo opaco após o cabeçalho: corrupção interna ainda pode escapar. A [estrutura oficial do cabeçalho](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html) define o tamanho e a codificação little-endian usados pela checagem. Incompatibilidade válida de cache já deve ser tolerada pelo Vulkan; a checagem não comprova um defeito no driver.

`tools/test_vulkan_startup.py <NDK-bin>` executa 33 casos das funções de produção no PC para composição, usos de superfície e cabeçalho/identidade do cache; os 21 testes existentes de features/filas também passaram. Não são testes de driver. Não há confirmação de que estas falhas expliquem algum aparelho da lista, nem certificado de 30 FPS em Mali. Vulkan 1.3 continua sendo o requisito do backend. A ausência de `shaderInt64`, inicialmente uma limitação sem fallback, recebeu posteriormente um caminho de 32 bits validado visualmente no S23; veja o [diagnóstico do driver padrão](ANDROID-S23-STOCK-RENDERING.md).

1. **Inicialização e tela preta:** obter versão do APK, modelo/Android, driver, etapa da falha e captura da mensagem. Não aplicar bloqueios por modelo com base nestes comentários.
2. **Desempenho nos aparelhos que abrem:** registrar modo Auto/30/60, resolução, antialiasing e upscaler. Comparar a mesma cena, após aquecimento semelhante, com duração e frametime, não apenas FPS instantâneo.
3. **Referências que funcionam:** coletar os mesmos dados nos casos positivos para permitir comparação.

## Soluções candidatas e ordem de trabalho

| Frente | Ação concreta | Limite da evidência |
| --- | --- | --- |
| Falha ao inicializar / tela preta | Consultar os bits das extensões opcionais antes de solicitá-los; registrar capacidades e mostrar o motivo fatal | Corrige solicitações potencialmente incompatíveis; não demonstra a causa de nenhum comentário |
| Falha antes de Vulkan | Identificar se parou na importação, criação da imagem de dados ou compilação do código; verificar espaço disponível e versão 1.0.0 | Um relato de tela preta sem etapa/log não deve ser tratado automaticamente como problema de GPU |
| Recursos obrigatórios ausentes | Identificar exatamente o recurso e avaliar um fallback correspondente | Reduzir o número da API exigida ou ignorar um feature bit não implementa o recurso usado pelo renderer |
| MediaTek com baixo FPS | Medir tempo de decodificação BC/ASTC e volume de uploads; se relevantes, reduzir conversões repetidas e avaliar decode por blocos/NEON ou trabalho em paralelo | O fallback BC por CPU já existe; a hipótese de gargalo precisa ser medida nesses aparelhos |
| 8s Gen 4 com queda ao subir resolução | Medir separadamente passes do jogo e composição/upscaling na mesma cena, após aquecimento semelhante | Os dois números de FPS sugerem investigar custo por pixel, mas não provam saturação da GPU |
| Configuração inicial | Oferecer presets explícitos de qualidade/desempenho, com resolução e modo de FPS visíveis, após validação | Evitar escolher um preset por nome de chip com esta amostra ou prometer 60 FPS |

Na revisão local, o Android passou a consultar `customBorderColors`, `customBorderColorWithoutFormat`, `vertexAttributeInstanceRateDivisor`, `deviceFault` e `pipelineExecutableInfo`. As extensões só são habilitadas para esses caminhos quando os respectivos bits permitem o uso. O sampler atual usa `VK_FORMAT_UNDEFINED`, portanto precisa dos dois bits de custom border: essa exigência está na [especificação Vulkan](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceCustomBorderColorFeaturesEXT.html).

Quando custom border ou divisor não estão disponíveis, o backend mantém os fallbacks já existentes: cores de borda padrão e divisor limitado a 1. Essas aproximações podem mudar a imagem/instanciamento; não são emulação completa desses recursos. Desativar device fault ou estatísticas de pipelines afeta somente os diagnósticos opcionais. As mudanças ainda precisam ser validadas nos drivers dos aparelhos relatados.

ADPF permanece no comportamento estável. Nenhuma otimização de FPS deve ser publicada como melhoria antes de uma comparação de frametime reproduzível. O caso Chain Chomp fica separado destes relatos: o mantenedor informou que ele apareceu ao iniciar um novo jogo; a causa no save antigo não foi confirmada.

Em 21/10/2026, uma simulação local com `LARECOMP_NO_BC=1` exercitou o fallback BC por CPU no POCO F7 Ultra/Adreno 830 com Turnip, usando uma cópia do save, 50% e FSR1. As três execuções chegaram a Field sem travar. Nas janelas de aproximadamente 59 s, o controle nativo marcou 56,57 FPS/p99 33,38 ms; o fallback, 58,85 FPS/p99 33,38 ms; e a repetição nativa, 59,51 FPS/p99 25,03 ms. A variação do controle impede atribuir ganho ao fallback. Ele permanece apenas diagnóstico quando forçado nesta GPU.

Na entrada do mapa, o fallback registrou 42,54 MiB de BC convertidos em 132,87 MiB de staging e 105,19 ms acumulados de decode em uma janela de 3,23 s. Isso mede trabalho CPU de carga, não um frame de 105 ms nem tempo GPU. Não houve novos lotes agregados na cena parada após a carga; o trace depende de uploads para emitir e pode reter uma janela parcial. A simulação não reproduz hardware/driver Mali, não exercita ASTC por CPU e não comprova a causa dos relatos. A captura do fallback mostrou somente a corrente do Chain Chomp, enquanto a última nativa mostrou o personagem; a intermitência continua sem causa confirmada e esta rodada não valida equivalência visual. APK e configuração originais foram restaurados, os saves reais tiveram hashes preservados e nada foi publicado.

O [formulário de bug](../.github/ISSUE_TEMPLATE/bug_report.yml) aceita relatos sem log. Uma captura da mensagem de erro já ajuda; arquivos do jogo e saves não devem ser anexados publicamente.

## Relato de renderização com o driver padrão

Em 21/10/2026, o mantenedor relatou no próprio celular: o corpo do Chain Chomp não aparece, mas a corrente permanece visível, usando o driver padrão. A cena exata, a versão do APK e a comparação da mesma cena com Turnip ainda precisam ser confirmadas. Não associar este relato às telas pretas da tabela.

Na mesma data, uma coleta por adb sem reiniciar o processo confirmou visualmente a corrente visível e o corpo ausente diante da casa na vila. APK instalado: 0.3.8; GPU: Adreno 830; driver proprietário Qualcomm 0800.71; Vulkan 1.3.284. A coleta registrou o caminho sem importação do heap, com memória global por cópias, e mensagens de acessos fora das tabelas que acionam o aprendizado de faixas. Não há associação desses acessos a um shader do Chain Chomp. Os resumos recentes indicaram `compile 0` e `0 skipped`, o que não comprova correção dos shaders nem elimina descarte de fragmentos/geometria. Evidências locais: `build/chain-chomp-system/screen.png` e `logcat.txt`. Nenhum input, instalação ou alteração de configuração foi enviado ao jogo nessa coleta. A comparação com Turnip permanece pendente.

A inspeção do código mostra caminhos diferentes para o driver proprietário Qualcomm: tradução de fluxo de controle dos shaders (`structure_on`, em `runtime/nvn/vulkan/exec.c`) e acesso à memória global com cópias quando não há importação do heap. Isso fornece pontos de investigação, não uma causa confirmada. Também é necessário verificar os shaders e o estado de profundidade/descarte do desenho do personagem.

Próxima reprodução: usar o mesmo save, posição, câmera e configurações com os dois drivers; confirmar se o personagem continua invisível após terminar a compilação dos shaders. Só então identificar os desenhos/shaders envolvidos e testar uma alteração isolada. Nenhum workaround gráfico ou ajuste de ADPF foi aplicado a partir deste relato.

Comparação realizada em seguida, com reinício autorizado e Turnip Gen8 V37: o corpo também permaneceu invisível, com a corrente visível na mesma área da vila, após resumos com `compile 0` e `0 skipped`. Houve pequeno deslocamento de posição/câmera; não foi comparação pixel a pixel nem benchmark. Resolução 50%, FSR1, AA desligado, modo Auto; overlay ADPF ativo em duas threads, alvo 13,3 ms, nos dois drivers. Ambos registraram uso de cópias para memória global sem importação do heap. O defeito, portanto, não é exclusivo do driver proprietário neste aparelho. Próxima etapa: identificar os desenhos/shaders do corpo e os dados/estados compartilhados pelos dois caminhos. Evidências em `build/chain-chomp-system/comparison.md` e `turnip-field.png`. Jogo deixado aberto com Turnip; nenhuma release ou APK novo instalado.

Investigação posterior: o corpo reapareceu com Turnip no APK de diagnóstico e também no APK público original, em controles com e sem gravação de shaders e após uma permanência longa na abertura. O diagnóstico não é necessário para o corpo aparecer; isso não comprova correção. Foram capturados 33 passes após corrigir localmente os caminhos PNG relativos no Android. Compararam-se 177 pares de SPIR-V em cache de fontes com/sem anotações: todos idênticos em bytes. O APK de diagnóstico foi temporário e o público foi restaurado. Os controles usaram cópia do save, sem alterar ADPF ou configurações gráficas. Falha continua sem causa confirmada; não publicar a correção da captura como correção do personagem.

O controle final com o driver padrão Qualcomm, APK público e entrada rápida também mostrou o corpo (`system-quick-control.png`). Classificar como falha intermitente observada nos dois drivers, sem atribuição exclusiva ao compilador Qualcomm ou à ausência de uma capacidade Vulkan. Reaparecer após reiniciar não comprova correção. Próxima investigação útil: obter identidade e conteúdo dos shaders/dados do desenho na mesma cena em uma execução boa e outra ruim, sem depender apenas de endereços de memória ou de um frame capturado com esperas extras.

Ao retornar ao save real sem override, a invisibilidade reapareceu; também ocorreu apontando para esses mesmos arquivos por caminho absoluto. Os quatro arquivos reais e os da cópia foram comparados diretamente no aparelho e eram idênticos. Em nova execução com leitura de diagnóstico do buffer de save em memória, o corpo apareceu tanto com o save real quanto com a cópia relativa. Os buffers carregados diferiam somente em três bytes, no campo em `0x53a0`; as 256 posições em bytes das flags de eventos eram idênticas. Não atribuir a causa ao caminho ou ao conteúdo do save: a leitura de diagnóstico e os reinícios podem alterar o tempo de execução. Nem erro de shader fixo nem falha de sincronização foram comprovados. Relatório local completo: `build/chain-chomp-system/comparison.md`.

## Diagnóstico local preparado

O backend Android consulta os recursos Vulkan 1.1/1.2/1.3 já exigidos pelo renderer antes de criar o dispositivo, identifica um recurso ausente e registra GPU, driver e capacidades com o prefixo `[vk-cap]`. A mensagem fatal passa a mostrar o motivo do erro, quando disponível, para facilitar uma captura sem adb.

Essa checagem não implementa emulação de recursos ausentes nem garante identificar toda tela preta: importação, shaders, extensões, apresentação e falhas do driver também precisam de evidência. O requisito de Vulkan 1.3 permanece. Solicitar um recurso não suportado pode falhar na criação do dispositivo, conforme a [referência de vkCreateDevice](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateDevice.html).

As mudanças são locais, compiladas no PC, ainda sem validação nos aparelhos relatados. O ADPF estável permanece inalterado. Não publicar otimizações de desempenho antes de comprovar melhora no frametime.
