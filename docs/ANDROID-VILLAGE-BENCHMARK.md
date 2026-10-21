# Benchmark de Mabe Village

Resultados das comparações e limitações observadas: [rodada de 21/10/2026](ANDROID-VILLAGE-RESULTS-20261021.md). Algumas repetições posteriores acumularam deriva; uma primeira execução válida não garante repetibilidade de todas as trajetórias.

Rota `mabe-village-v1`, definida em [village-route.json](../tools/benchmarks/village-route.json) e executada por [android_village_benchmark.py](../tools/android_village_benchmark.py). Usa apenas o analógico: não conversa, ataca, entra em casas ou altera o progresso real. O ponto de partida é o caminho diante da casa do Chain Chomp, posição de diagnóstico `(30.75, 10.50, 131.25)` em `Field`. É necessário um snapshot privado do autosave nessa posição; um save de outro ponto não é intercambiável.

| Trecho de cada volta | Movimento | Pausa |
| --- | ---: | ---: |
| Rua para oeste | 120 frames | 120 frames |
| Rua para leste, cruzando o ponto inicial | 240 frames | 120 frames |
| Volta ao centro | 120 frames | 120 frames |
| Gramado ao norte, entre as casas | 60 frames | 120 frames |
| Gramado ao sul, cruzando a rua | 120 frames | 120 frames |
| Volta ao ponto inicial | 60 frames | 120 frames |

Quatro voltas totalizam 5.760 frames: 96 segundos nominais a 60 FPS, metade em movimento e metade em pausas. O aquecimento termina no frame 4.200 desde o lançamento, aproximadamente 70 segundos a 60 FPS, incluindo a navegação inicial. **Os tempos reais aumentam quando o FPS cai.** Não tratar o número de frames como duração fixa. A rota é restrita ao miolo da vila; não cobre todas as áreas, interiores, combate ou transições de mapas.

## Execução

Compilar o APK de diagnóstico com os marcadores opcionais de `runtime/hle/input.c`. Salvar e fechar o jogo antes do teste. O comando `plan` não acessa o aparelho:

```powershell
python tools/android_village_benchmark.py plan
python tools/android_village_benchmark.py run `
  --adb 'C:\Program Files\platform-tools\adb.exe' --serial SERIAL `
  --apk android/lasrecomp/app/build/outputs/apk/release/app-release.apk `
  --snapshot build/SEU-SNAPSHOT-PRIVADO `
  --out build/village-benchmark-RODADA
```

O snapshot deve conter `save_0/savedata3.bin` e os demais arquivos de save correspondentes. Preservá-lo entre comparações. A pasta de saída deve ser nova. Não publicar saves, arquivos do jogo, backups de APK ou dumps privados junto dos resultados.

A ferramenta recusa iniciar se o app já estiver aberto. Faz backup do APK instalado, configuração e save real; copia o snapshot para uma pasta de teste independente; instala temporariamente o diagnóstico; executa a rota e restaura APK/configuração em `finally`, verificando os hashes do save real e do APK restaurado. Deixa o app fechado e mantém os backups. Uma interrupção do processo Python/PC, desconexão ou falha do ADB pode impedir a restauração: nesse caso os backups permanecem na pasta de saída. Não usa desinstalação e não modifica o driver selecionado.

O ADPF mantém o comportamento padrão estável. A ferramenta não fixa clocks, resolução, modo de FPS ou upscaler; registra o hash da configuração e o log de capacidades para comparação. Manter 50%/FSR1/AA desligado/Auto durante a referência atual e as mesmas condições de tela, driver, versão e temperatura entre variantes. `--cpu-bc` acrescenta somente o teste de fallback BC; os dois caminhos usam o mesmo trace de texturas. Esse teste não simula o hardware de outro chip.

`--variant control|descriptor|upload|combined` seleciona somente as flags experimentais do plano de descritores e/ou barreira inicial de upload. Não aceita combinar esses experimentos com `--cpu-bc`. A seleção fica registrada no manifesto junto ao ambiente completo, hash do APK e hashes do snapshot. O controle mantém ambas as flags desligadas. Não há override de ADPF. Antes/depois de cada rodada, `thermal-*.txt` registra o serviço térmico; distinguir as leituras atuais da HAL dos valores antigos em cache. Estado térmico 0, sozinho, não prova clocks iguais nem ausência de limitações do fabricante.

## Medição e validade

`LARECOMP_BENCHMARK_MARKERS` registra limites no thread de input/jogo usando `CLOCK_MONOTONIC`, o mesmo domínio dos timestamps de apresentação do SurfaceFlinger. São limites CPU, não conclusão GPU; frames em trânsito podem cruzar o limite de um trecho. A coleta de apresentação não insere queries ou esperas no renderer.

O resultado contém FPS médio, p50/p95/p99, máximo e proporção acima de 20 ms, além das métricas de cada trecho/volta. Só usa pares adjacentes observados juntos no histórico do SurfaceFlinger; lacunas invalidam a rodada, em vez de transformá-las em um frame longo inventado. Os dados brutos ficam em `surface.jsonl` e `logcat.txt`. O método não distingue uma repetição de imagem de um novo frame de simulação do jogo.

Nas comparações posteriores à validação inicial, os marcadores também delimitam o fim de cada movimento, permitindo separar `moving` e `paused` em `result.json`, além das voltas em `loops`. A duração coberta exclui os intervalos que cruzam o limite CPU de cada janela. Marcadores não criam sincronização de apresentação; são aproximações por janela para comparação, não associação exata entre cada comando de input e o frame exibido. A ferramenta retorna erro em uma rodada inválida e preserva o diagnóstico, restaurando o APK/configuração no encerramento normal desse erro.

Uma tentativa posterior encontrou a superfície inicial substituída pelo Android: o histórico dessa camada parou antes do início da rota. Nenhum FPS foi atribuído a essa tentativa sem amostras. A coleta agora redescobre a camada após três amostras sem avanço dos timestamps, prefere o identificador mais recente entre as superfícies do app e registra as escolhas em `layers.jsonl`. A validação final continua exigindo dados dentro dos marcadores e ausência de lacunas; trocar de camada não torna uma captura incompleta válida.

`LARECOMP_LOG_LINK=60` confere a posição inicial e o final de cada trecho, já durante a pausa. Um desvio acima de 1 unidade, ponto ausente, outro mapa ou lacuna de histórico resulta em `valid_route: false`. A partida é por frames; mudanças de ritmo da simulação, NPCs, colisões e animações ainda podem produzir trajetórias diferentes. Essa checagem rejeita tais divergências, mas não prova equivalência visual nem confere todos os pontos intermediários. Em aparelhos muito mais lentos, pode ser necessário calibrar outro protocolo antes de comparar resultados.

Comparar pelo menos A/B/A ou mais repetições, com o mesmo snapshot e condições térmicas semelhantes. Separar primeira visita de voltas já aquecidas quando houver compilação de pipelines. Um único resultado válido estabelece uma referência de percurso, não comprova uma otimização. Não publicar ganho sem melhora repetível de frametime e qualidade equivalente.

## Validação inicial — 21/10/2026

Executada no POCO F7 Ultra/Adreno 830 com Turnip Mesa 26.3.0-devel (pacote V37), 50%, FSR1, AA desligado e Auto. As quatro voltas completaram os 24 pontos de controle mais a posição inicial, dentro da tolerância de 1 unidade. Não houve troca de mapa ou lacuna no histórico. A posição final foi `(30.62, 10.50, 131.99)`, a aproximadamente 0,75 unidade do início: há pequena deriva acumulada, não retorno matematicamente exato.

| Medida | Referência inicial |
| --- | ---: |
| Duração coberta | 96,67 s |
| FPS médio | 59,58 |
| p95 | 16,65 ms |
| p99 | 24,97 ms |
| Máximo | 41,61 ms |
| Intervalos >20 ms | 1,74% |

Esta é uma rodada de validação do percurso, sem variante de otimização. A falha anterior da ferramenta ao consultar o PID antes de o processo Android existir foi corrigida com espera limitada de inicialização; aquela tentativa foi restaurada automaticamente e não produziu benchmark. Dados locais da rodada válida em `build/village-benchmark-validation-20261021b/`. O APK público original foi restaurado com SHA256 `d252c602eb345a33b05550bcd40fa4bfd3dd369b2fd20d1f77cfe080cf0bbb95`; configuração restaurada, saves reais verificados sem alteração e app deixado fechado. Nada foi publicado.
