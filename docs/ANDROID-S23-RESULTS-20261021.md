# Galaxy S23 — inicialização e desempenho em 21/10/2026

O caminho padrão de memória do Turnip foi encerrado pelo Android por falta de memória, inclusive após o usuário fechar os demais aplicativos. O fallback existente, ativado temporariamente com `LARECOMP_VK_NO_PLACED=1`, iniciou e completou três amostras na vila, próximas de 30 FPS. Isso valida uma alternativa de inicialização nesta configuração, mas não comprova frametime perfeitamente estável nem compatibilidade em outros aparelhos.

## Configuração e método

- Galaxy S23 SM-S911B, Adreno 740, Android 16, 8 GB de RAM anunciados; `/proc/meminfo` informa 7.242.456 KiB utilizáveis pelo sistema. Após fechar os aplicativos, `MemAvailable` era 2.560.324 KiB, aproximadamente 2,44 GiB.
- Turnip v26.0.0 R8 normal, Mesa 26.0.0-devel, git `5ac41be677`, Vulkan 1.4.335.
- APK local 0.3.8/code 12, SHA256 `51ede5b0cd8a6cbdee9d9552404c67861324b15f930f6dfe74b1a1494f86c2fc`. Não é a mesma compilação da release pública com esse nome de versão.
- 30 FPS fixos, FSR1, AA desligado, aspecto original; resolução na ordem 50%, 75%, 50%. Mesmos arquivos de save copiados novamente antes de cada execução, em pasta isolada.
- Cena parada em Mabe Village, posição `(30.75, 10.50, 131.25)`, confirmada pelos logs. Não foi um percurso em movimento.
- Entrada automatizada, espera pela chegada ao mapa e 30 segundos de aquecimento. Cada coleta durou aproximadamente 90 segundos, cobrindo cerca de 89 segundos de intervalos observados.
- Timestamps de apresentação do SurfaceFlinger; intervalos adjacentes deduplicados, sem lacunas no histórico. A leitura informou período de tela de 8.333.333 ns. Os números abaixo medem entrega dos frames à tela, não duração de comandos na GPU.
- ADPF sem alterações: sessão de duas threads e alvo de 13,3 ms nas capturas. Flags de descritores, barreiras experimentais, afinidade e hints de GPU não foram habilitadas.

## Falhas antes da medição

As primeiras duas tentativas com o caminho padrão tiveram `ApplicationExitInfo reason=3 (LOW_MEMORY)`. A repetição após o usuário fechar os aplicativos também terminou com `LOW_MEMORY`, às 13:38:30. Não chegou à cena de medição; portanto não há FPS válido para o controle padrão. O primeiro log registrou o heap de 3.220 MiB mapeado como memória GPU. O RSS do processo relatado na saída não contabiliza necessariamente toda a pressão de memória do driver/sistema.

Uma tentativa anterior com fallback foi removida dos recentes durante a liberação de RAM (`USER REQUESTED / REMOVE TASK`) e foi excluída. Não foi contada como falha do fallback.

Depois disso, as três execuções com `LARECOMP_VK_NO_PLACED=1` completaram a carga e a medição. Essa flag evita mapear todo o heap do jogo como memória GPU e usa o caminho de cópias existente. A associação com a falha de memória merece investigação; estes testes não isolam todos os custos ou o mecanismo interno do driver e não demonstram ganho de desempenho contra um controle que executa normalmente.

## Amostras válidas

| Ordem | Resolução | Intervalos | FPS médio | p50 | p95 | p99 | Máximo | Acima de 35 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 50% | 2.677 | 30,01 | 33,37 ms | 41,71 ms | 41,72 ms | 58,40 ms | 6,87% |
| 2 | 75% | 2.678 | 29,98 | 33,37 ms | 41,72 ms | 41,72 ms | 50,06 ms | 13,11% |
| 3 | 50%, repetição | 2.673 | 30,00 | 33,37 ms | 41,71 ms | 41,72 ms | 50,06 ms | 5,57% |

O orçamento ideal de 30 FPS é 33,33 ms; o limiar de atraso usado foi 35 ms, com tolerância de 5%. A média ficou próxima do alvo nas três rodadas, mas os intervalos longos impedem descrever o frametime como perfeitamente estável. A amostra de 75% teve mais intervalos atrasados que ambas as de 50%; é um indício para preferir 50% como ponto de partida neste cenário, não uma caracterização completa do aparelho.

O serviço térmico informou estado 0 antes/depois das três amostras. Na HAL, a temperatura AP foi de 50,8 para 48,2 °C na primeira; 47,7 para 47,5 °C na segunda; e terminou em 46,7 °C na última. Isso não garante clocks iguais nem ausência de limitação pelo fabricante. O aparelho estava conectado por USB, com bateria subindo durante a série. Não é teste de autonomia nem de carga térmica prolongada.

As capturas finais das três amostras foram inspecionadas: a vila, Link e o corpo do Chain Chomp estavam visíveis. Isso é uma verificação pontual, não validação visual completa do renderer. Não foram testados outros mapas, combate, movimento, 60/120 FPS ou GPUs Mali nesta série.

## Estado ao terminar e próximos passos

O aplicativo foi encerrado e reaberto normalmente no launcher, sem extras de teste. O APK e o Turnip R8 foram mantidos; o fallback temporário não foi persistido. A pasta de save real continuou ausente, como antes dos testes; a partida medida usou apenas a cópia isolada. Nenhuma alteração de ADPF, publicação ou push foi feito.

Prioridade técnica: investigar a reserva/residência do heap no caminho de mapeamento GPU e uma seleção segura do fallback por capacidade/orçamento de memória, antes de tentar aumentar FPS. Validar depois uma rota em movimento e outras cenas, repetindo inicializações; o fallback ainda não está habilitado por padrão e o launcher normal pode reproduzir a falha original ao iniciar o jogo.

Evidências locais em `build/s23-perf-20261021/`: `freed30-50-a-exit.txt`, `freed-memory-before.txt`, e as pastas `freed-copy30-50-a`, `freed-copy30-75`, `freed-copy30-50-b`, com `result.json`, `surface.jsonl`, logs, estado térmico e captura final. Arquivos de save e logs completos permanecem locais; não publicar a pasta inteira.
