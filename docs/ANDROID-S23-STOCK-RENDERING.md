# S23: iluminação, grama e textos no driver padrão

Em 21/10/2026, a bateria de FPS foi interrompida a pedido do usuário quando o driver padrão do Galaxy S23 apresentou renderização incompleta. Somente as 40 amostras já concluídas com Turnip foram incluídas no README. As medições parciais do driver padrão ficaram fora da tabela.

## Diagnóstico e correção local

O SM-S911B/Adreno 740 com Android 16 informou Vulkan 1.3.128, driver Qualcomm `3df4c357f0, I515341007c, 1769513954`, e `shaderInt64=0`. O tradutor ainda gerava aritmética de 64 bits para endereços de memória global, carry de somas e deslocamentos combinados. A ausência do recurso era apenas registrada como aviso. Não se deve depender do resultado de shaders que exigem uma capacidade não suportada.

O backend passou a guardar o feature bit e selecionar a tradução equivalente com palavras de 32 bits quando necessário. O novo caminho mantém ambas as metades do endereço até somar o deslocamento com sinal e subtrair a base do heap, valida a metade alta e somente então usa o offset relativo de 32 bits. Somas preservam carry de entrada/saída; deslocamentos tratam explicitamente o caso zero. Heap acima de 4 GiB e operações/literais largos não convertidos são rejeitados na tradução, em vez de truncados silenciosamente. O heap deste jogo é limitado a 3,25 GiB.

Drivers que anunciam `shaderInt64`, como o Turnip R8 medido neste S23, mantêm a geração nativa. O ADPF não foi alterado. A flag de diagnóstico `LARECOMP_VK_INT32=1` permite forçar o caminho de 32 bits em um driver com suporte nativo; ela não foi necessária no Qualcomm.

Uma tentativa prévia com a flag existente de controle de fluxo estruturado (`LARECOMP_STRUCTURE=1`) não recuperou a imagem. Essa configuração não foi habilitada por padrão como parte da correção.

## Verificação

Mesmo snapshot privado em Mabe Village, posição `(30.75, 10.50, 131.25)`, 30 FPS fixos, 50%, FSR1, AA desligado, 16:9, driver padrão e sem alterações de ADPF. Foi mantida a mesma flag de memória `LARECOMP_VK_NO_PLACED=1`; o driver proprietário não oferece o mapeamento colocado usado pelo Turnip.

- Duas cargas do APK corrigido recuperaram a iluminação e as touceiras de grama.
- No inventário corrigido, os textos `Items`, `Map`, `System`, `Set item`, `Back` e o contador de rupias ficaram visíveis.
- Um controle posterior com o APK anterior, mesmas opções e mesmo save, reproduziu a iluminação escura, ausência das touceiras e dos textos/contador do inventário. Isso separa o resultado de uma simples troca de Auto para 30 FPS.
- Build Android de release concluído. No PC, 62 casos de instruções geraram 186 shaders nos estágios vertex/fragment/compute; a saída Metal permaneceu igual ao alternar a flag. Foram executados 253.952 vetores dos cálculos efetivamente emitidos, contra um oráculo `uint64_t`, com verificação de comportamento indefinido e limites do heap.
- Os 186 shaders sintéticos compilaram sem capability Int64 nem tipos inteiros de 64 bits no SPIR-V. O glslc antigo do SDK não entende a extensão de shadow LOD já existente no projeto: somente para esses testes, que não usam shadow samplers, a declaração dessa extensão foi omitida. O shaderc do aplicativo compilou os shaders reais completos nas duas cargas.
- Os 12 testes de limites das tabelas de constantes e os 17 testes do cache de shaders também passaram.

Não foi retomada a bateria de FPS, nem comprovado ganho de desempenho ou equivalência gráfica completa em outros mapas/aparelhos. O problema intermitente do corpo do Chain Chomp continua separado: esta correção não é apresentada como solução desse caso. GPUs Mali ainda precisam de verificação real.

APK de diagnóstico: SHA256 `15a6858f75cb1b6bc796c8f927f2b004d04f0f525665ca2c60243eb9c654aba4`. APK anterior restaurado após as checagens: SHA256 `51ede5b0cd8a6cbdee9d9552404c67861324b15f930f6dfe74b1a1494f86c2fc`. Nenhuma release ou push foi feito.

Evidências locais: `build/s23-stock-render-20261021/int32`, `int32-repeat` e `stock-control`, com `field.png`, `menu.png` quando aplicável e `log.txt`. O controle de fluxo estruturado está em `structured.png` e `structured-log.txt` na pasta pai. Os backups e a telemetria completa permanecem locais.

Para reproduzir os testes de cálculo em Linux: compilar `tools/test_shader_int32.c` com `cc -O2 -lm`, executar o gerador indicando a pasta de saída, compilar o `math.cpp` emitido com `c++ -O2 -fsanitize=undefined` e executá-lo. `tools/test_shader_int32.py` automatiza esse processo e a checagem SPIR-V; `--generated-only` permite validar saídas produzidas em outro ambiente, e `--legacy-shadow-extension` documenta a adaptação limitada para o glslc antigo do SDK.
