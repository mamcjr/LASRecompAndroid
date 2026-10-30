# Código nativo do jogo (Android, experimental)

Com **Código do jogo: Nativo** (a escolha acima da pasta na página dos arquivos do jogo) ou **Vídeo > Código nativo do
jogo**, o código ARM64 original do jogo roda direto na CPU do celular, no lugar do `lasgame.so` que a configuração
compila a partir dos arquivos do jogador. A configuração então só copia os arquivos: sem os minutos de compilação, sem
a memória e o espaço que ela pede. `LARECOMP_NATIVE=0/1` passa por cima da configuração (testes). Desligado por
padrão. Veio do BOTWRecompAndroid (`runtime/native.c`, `runtime/native_gate.S`), que seguiu a ideia do MacNativeGames e
do reescritor do d2r-mobile (`patch_nso.py`).

## Como funciona

O `main` e o `sdk` são carregados e relocados nos endereços do guest como sempre (`loader.c`); como endereço do guest é
endereço do host, o código deles pode rodar ali como está. No boot (`native_prepare`, depois do `rando_boot`) só o que não
roda fora do Switch é reescrito, no lugar:

| No código do jogo | Vira |
| --- | --- |
| `svc #n` | um desvio para um stub depois do módulo, que entra em `native_svc_gate`: os registradores vão para o `Cpu` da thread, o `rt_svc` roda na pilha do host, os registradores voltam |
| `mrs`/`msr tpidr_el0`, `mrs tpidrro_el0` | o slot do guest no `Cpu` (o `TPIDR_EL0` real é o TLS da bionic); o `Cpu` vem de `g_native_tab` pelo `TPIDR_EL0` |
| `mrs cntpct_el0` | `cntvct_el0` onde o contador é de 19,2 MHz (Snapdragon, o mesmo do Switch); nos outros (MediaTek, Exynos, Tensor) um stub o escala para 19,2 MHz, e `mrs cntfrq_el0` passa a ler 19,2 MHz |
| as palavras de código de uma seed | escritas no próprio código (o código gerado pergunta ao `rando_word`) |
| os 68 pontos do `tools/patches.py` | stubs que entram em `native_patch_gate`: o mesmo C dos patches (widescreen, ritmo dos quadros, randomizer, as correções de crash), no `Cpu`, que diz como seguir |

As funções do runtime (HLE: NVN, os callbacks do rtld, as importações) continuam nos endereços falsos de `HLE_BASE`,
agora código: cada slot de 16 bytes salta para `native_hle_gate` com o seu índice. `guest_call` entra pelo
`native_enter`. `kernel.c`, serviços, NVN e Vulkan não mudam. O `x18` fica com o jogo (o Linux o preserva); as portas
devolvem o do host enquanto o C roda, e o tratador de sinais também, quando a falha vem do código do jogo.

Cada patch confere a instrução original antes de trocar (`tools/test_android_native.py` confere a tabela contra o
`main` da 1.0.0, da 1.0.1 e da 1.1.0 em `ROM/`) e responde um de quatro caminhos: rodar a instrução original (um `b`/`bl`
reapontado a partir do stub), o outro endereço do patch (o `goto` do `patches.py`), `ret`, ou seguir sem ela. O quadro do
stub na pilha do guest é reescrito a partir do `Cpu` depois do C: o handler pode mover o `sp`, mudar o `x30` ou chamar o
jogo (`guest_call`) por cima dele. As correções "o código recompilado não desempilha exceções" (`NO_HANDLER_SITES`,
`MISSING_*`, `EMPTY_HANDLER_SITE`) continuam: o comportamento fica o mesmo do recompilado; o início e o fim do catch só
são pulados depois do salto do próprio patch (aqui um `throw` do jogo pode chegar ao mesmo catch).

Uma falha dentro do código do jogo copia os registradores do sinal para o `Cpu`: o relatório mostra os do jogo, o pc
como `main+offset` com o símbolo do módulo e a cadeia de chamadores pelo `x29`.

## Estado

- Compila e os stubs saem como esperado no PC (`tools/test_android_native.py`: os codificadores do `native.c`
  desmontados, e os 68 pontos contra o jogo). **Ainda não rodou num celular.**
- No BOTWRecompAndroid o mesmo caminho chegou à tela de título e ao mundo no POCO (Adreno), sem ganho de fps; o ganho
  é não compilar. O que vale testar aqui: a tela de título, o vilarejo, um reinício para a tela inicial, uma seed do
  randomizer, o widescreen, e um Mali (o contador escalado e o rastreio de escrita de páginas com o código do jogo).
- Riscos: memória executável anônima (`mprotect` com `PROT_EXEC`; permitida a apps, o caminho dos JITs, mas cada
  fabricante pode ter política própria); páginas de 16 KB (o fim do texto é arredondado para a página do sistema); o
  tratador de falhas do ART roda antes do nosso com o `x18` do jogo.
