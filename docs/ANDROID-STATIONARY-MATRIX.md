# Matriz de desempenho parado na vila

Protocolo para comparar configurações e drivers, sem limite mínimo de FPS para aceitar uma amostra. Não é teste de melhoria de uma otimização nem substitui uma rota em movimento.

**Execução de 21/10/2026 no S23:** completadas as 40 combinações com Turnip R8. A execução com o driver padrão foi interrompida a pedido do usuário após observar renderização incompleta: iluminação e grama ausentes nas capturas, além de textos ausentes relatados pelo usuário. Seus resultados parciais não fazem parte da tabela do README. Os logs e capturas ficaram preservados localmente para diagnóstico. O APK anterior e a preferência de tela ligada foram restaurados; o teste do POCO foi adiado para priorizar a compatibilidade do driver padrão.

## Perfis

FPS em **Auto** em todos os perfis. No código atual, Auto permite ao jogo alternar entre seus modos de 30 e 60 FPS; não é um desbloqueio para 120 FPS.

Produto completo das opções:

- Resolução: 50%, 75%, 1x, Dynamic e Match Display; acrescentar 2x no POCO F7 Ultra.
- Upscaling: bilinear ou FSR1. “Sem FSR1” significa bilinear, não ausência de composição da imagem.
- Anti-aliasing: Off ou SMAA.
- Aspecto: Original 16:9 ou Unlocked.
- Driver: Turnip instalado ou Vulkan padrão do aparelho.

São 40 perfis por driver no S23 (80 amostras), e 48 por driver no POCO com 2x (96 amostras). Duas execuções do jogo por aparelho: uma por driver, com troca de configuração ao vivo. A ordem é igual nas duas execuções: resolução, aspecto, upscaler, AA, conforme o manifesto gerado pela ferramenta.

## Procedimento

Usar o mesmo APK de diagnóstico, snapshot privado do save e posição `(30.75, 10.50, 131.25)` em Mabe Village. O jogador fica parado; NPCs e animações continuam ativos. Sem movimento/inventário/menus durante a medição. Cópias e configurações separadas preservam a partida e preferências reais.

Após carregar Field, aquecer por 30 segundos. Para cada perfil, enviar um comando completo por arquivo temporário seguido de rename; o runtime consulta apenas quando `LARECOMP_BENCHMARK_CONFIG` foi explicitamente definido, valida os valores, drena o trabalho de renderização enfileirado, aplica as opções e registra uma confirmação com relógio monotônico. O comando não grava o INI real. A medição só começa após confirmar os valores aplicados, esperar 10 segundos e coletar o estado inicial. Registrar apresentação por 30 segundos; coletar estado final e screenshot fora dessa janela.

O ADPF mantém a implementação e os parâmetros estáveis. Sem mudanças de afinidade, clocks, resolução/taxa do display ou flags experimentais de descritores/barreiras. Para evitar os encerramentos de memória observados no S23, as duas execuções usam `LARECOMP_VK_NO_PLACED=1`, o caminho existente de cópias em vez de mapear o heap inteiro como memória GPU. Esta condição deve acompanhar os números no README; os resultados não representam o caminho padrão do Turnip na release pública.

A ferramenta impede o desligamento automático enquanto conectado por USB e restaura a preferência anterior ao terminar. Não remove bloqueio de tela. Restaura também o APK anterior e reabre o launcher sem os parâmetros de teste. A preferência permanente de driver não é alterada: a execução padrão é selecionada por override temporário.

## Dados e validade

- FPS médio: quantidade de intervalos de apresentação dividida pelo tempo efetivamente coberto por esses intervalos, com timestamps do SurfaceFlinger. Deduplicar intervalos adjacentes e redescobrir a camada se ela parar de atualizar. A cobertura costuma ser um pouco menor que os 30 segundos de coleta.
- Guardar também p50/p95/p99/máximo, timestamps brutos, confirmação do perfil, posições, logs e captura final por perfil. Abaixo de 30 FPS continua sendo resultado válido. Falta de frames, lacunas de observação, mudança de posição ou encerramento do jogo são falhas de coleta e devem aparecer como tal, nunca como FPS zero inventado.
- Logs de dois em dois segundos: contador do renderer, queries GPU existentes, esperas de apresentação/fences e resolução efetiva calculada pelo renderer. Em Dynamic, guardar fator/variações; o nome da opção não define uma resolução fixa.
- Telemetria a cada cinco segundos: memória do sistema, carga e clocks CPU/GPU quando o Android permite ler esses contadores. Erros de acesso ficam registrados; não interpretar contador inacessível como zero. Dumps térmicos, memória do processo e bateria antes/depois ficam fora da janela cronometrada.
- As telas finais verificam posição/configuração e defeitos evidentes, mas não comprovam equivalência visual completa de todo o renderer.

As configurações compartilham estado de Auto, histórico de Dynamic, caches e temperatura dentro de cada execução. Há aquecimento por perfil, mas não resfriamento até temperatura igual nem ordem aleatória. A ordem dos drivers e o carregamento por USB também podem influenciar os resultados. Esta matriz é uma exploração de desempenho; diferenças pequenas precisam de repetições alternadas antes de atribuir ganho causal.

O README mostra apenas a comparação de FPS por configuração e contexto de medição. Telemetria e backups ficam no diretório local de saída: não publicar automaticamente APKs, saves, screenshots privados ou logs completos.

## Ferramenta

`tools/android_stationary_matrix.py plan` lista os perfis sem acessar o aparelho. `--include-2x` acrescenta os oito perfis de 2x por driver.

`run` exige `--serial` e `--out`, que deve ser uma pasta nova; aceita `--snapshot`, `--apk`, `--settle` e `--measure`. O APK precisa conter os hooks opt-in `LARECOMP_BENCHMARK_CONFIG` e `[matrix-scale]`. O manifesto registra hashes do APK instalado, do APK de teste e dos arquivos do snapshot. `restoration.json` informa o resultado da restauração, inclusive em falha/interrupção.

`tools/report_stationary_matrix.py <saída> --turnip-only --write` exporta as 40 amostras completas do Turnip para os dois READMEs e para o JSON de FPS, excluindo a parte interrompida do driver padrão. Sem essa opção, o exportador exige a matriz completa com os dois drivers.
