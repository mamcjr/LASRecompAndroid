# Comparação de otimizações em Mabe Village — 21/10/2026

**Nenhuma melhoria de frametime ficou comprovada nesta série.** A barreira de uploads apresentou resultados intermediários mais consistentes que os controles, mas não superou o melhor controle. O plano de descritores não mostrou ganho na única execução válida; as tentativas de repetição foram rejeitadas. Ambas as opções continuam desligadas por padrão. Nada foi publicado.

## Método

POCO F7 Ultra, Snapdragon 8 Elite/Adreno 830, Android 16, Turnip Mesa 26.3.0-devel (pacote V37). Rota [`mabe-village-v1`](ANDROID-VILLAGE-BENCHMARK.md), quatro voltas, 50%, FSR1, antialiasing desligado e Auto. Mesmo APK, configuração e snapshot privado do save em todas as tentativas, conferidos por hash. O save real não foi usado para gravar o percurso. ADPF manteve o sinal de tempo de parede e alvo de 13,3 ms; sem override de GPU/CPU timing, clocks, afinidade ou fila.

O controle desliga as duas flags experimentais. A variante de descritores habilita somente `LARECOMP_VK_DESCRIPTOR_PLAN=1`; a de uploads habilita somente `LARECOMP_VK_UPLOAD_BARRIER=1`. A combinação não foi testada. Instrumentação de texturas, posição e marcadores CPU igual nas variantes; timestamps de apresentação coletados pelo SurfaceFlinger. Nas capturas válidas houve 5.759 intervalos observados, aproximadamente 96–98 segundos, sem lacunas e com os 25 pontos de posição dentro da tolerância de 1 unidade.

Houve interrupção solicitada pelo usuário e retomada posterior. Portanto, a série não teve apenas intervalos uniformes de 20 segundos nem temperaturas equalizadas. O serviço térmico informou estado 0 antes/depois das rodadas válidas, mas a leitura atual de superfície da HAL variou aproximadamente de 31,2 a 39,2 °C. Isso não prova clocks idênticos ou ausência de política do fabricante. Valores antigos em cache do serviço não foram usados como temperatura atual.

APK diagnóstico: SHA256 `c036f68b7fd9d428a6b23fd02d8e2d5c9abf3d46e8896415b55e75bbc36d960f`. Configuração: SHA256 `0e6efbef3f8790626eef7ac103700e0141e278ca92f5c26185079c693b800baf`.

## Rodadas válidas

| Rodada | Variante | FPS médio | p99 | Intervalos >20 ms |
| --- | --- | ---: | ---: | ---: |
| 1 | Controle | 59,11 | 33,27 ms | 3,70% |
| 2 | Descritores | 58,98 | 33,28 ms | 3,70% |
| 3 | Uploads | 59,68 | 24,96 ms | 1,96% |
| 4, repetida após a pausa | Controle | 59,92 | 16,67 ms | 0,73% |
| 5 | Uploads | 59,49 | 25,01 ms | 2,66% |
| 7 | Controle final | 59,25 | 33,23 ms | 3,65% |

A variação entre controles é maior que o benefício que se poderia atribuir às flags. Uploads ficou melhor que os controles 1 e 7, mas pior que o controle 4; isso é um indício para investigação, não um ganho isolado estabelecido. Descritores igualou aproximadamente a cauda do primeiro controle e teve um máximo isolado de 99,83 ms. Não atribuir esse pico a uma regressão da flag sem reprodução válida.

As primeiras passagens ainda registraram criação de pipelines ao revelar novas partes da vila: aquecer parado não aquece todo o percurso. As voltas finais também não resolvem a incerteza. Por exemplo, os p99 das voltas 3/4 foram 33,27/33,27 ms no controle 1; 16,67/16,67 ms no controle 4; 33,23/33,23 ms no controle 7; 16,64/24,96 ms em uploads 3; e 25,01/25,01 ms em uploads 5. Os resultados por trecho, movimento/pausa e volta estão nos arquivos locais `result.json`.

As capturas finais foram inspecionadas para os controles inicial/final e uploads 3. São verificações pontuais, não prova de equivalência visual de todo o percurso ou de todos os drivers. Na rodada 3, o áudio reportou `AAUDIO_ERROR_DISCONNECTED` e retomou; essa interferência também impede atribuir um pico isolado ao renderer.

## Tentativas excluídas e limite do protocolo

- O controle 4 original excedeu a tolerância de posição. A tentativa seguinte foi interrompida a pedido do usuário e restaurada; a retomada produziu o controle válido da tabela.
- A repetição 6 de descritores e sua tentativa adicional excederam a tolerância de posição. Não foram usadas para comparar desempenho. Não se aumentou a tolerância após observar os resultados.
- Na tentativa adicional 6, a superfície de inicialização foi substituída e seus timestamps pararam antes dos marcadores da rota. Não houve amostras utilizáveis na janela; isso não significa zero FPS. A coleta passou a redescobrir a superfície após três amostras sem avanço, com escolhas registradas em `layers.jsonl`. O controle final teve dados dentro dos marcadores e zero lacunas.
- Uma falha transitória de cópia do ADB impediu a primeira tentativa 2 antes da medição. A cópia foi repetida; erros posteriores de `push` passam a ser registrados e recebem até três tentativas limitadas.

O movimento comandado por frames acumulou deriva em algumas execuções. O teste de posição detecta esse problema, mas rejeitar trajetórias pode selecionar execuções mais estáveis. As tentativas rejeitadas estão preservadas, e a única rodada válida de descritores não sustenta uma conclusão de desempenho. Para testar diferenças pequenas com mais rigor, o próximo protocolo deve controlar os pontos de passagem sem depender somente da contagem de frames, registrar colisões e aquecer o percurso completo. Não inferir que a flag causou os desvios, nem reutilizar intervalos de trajetórias inválidas como evidência favorável.

## Correção de estabilidade encontrada antes da série

O primeiro controle, em um APK anterior, caiu na tradução de compute antes de iniciar o percurso. O backtrace foi simbolizado em `brx_table`, lendo constantes além da cópia que continha apenas o código do shader. A correção limita a análise ao tamanho real das constantes e inclui até 4 KiB dessas constantes na cópia de compute. Ela está presente em controles e variantes da tabela; não comparar esses resultados diretamente ao APK anterior para alegar ganho de FPS.

Os 12 testes de limites de [`test_shader_constants.py`](../tools/test_shader_constants.py) passaram. Recolocar a varredura antiga faz o teste de fronteira falhar por acesso fora da memória. A compilação Android release passou. Nenhuma das tentativas posteriores registrou novamente aquela queda; isso não garante ausência de outras falhas de shader. Detalhes em [otimizações Android](ANDROID-OPTIMIZATIONS.md).

## Encerramento

O APK público original foi restaurado, SHA256 `d252c602eb345a33b05550bcd40fa4bfd3dd369b2fd20d1f77cfe080cf0bbb95`, assim como a configuração. Os saves reais foram comparados por hash com os backups sem alteração. O app ficou fechado e não há processo de benchmark ativo. Nenhum push ou release foi realizado.

Dados e tentativas locais: `build/village-optimization-20261021/`, incluindo `summary.json`, manifestos, logs, superfícies, estados térmicos e comprovantes de restauração. Essa pasta contém backups privados e não deve ser publicada integralmente.
