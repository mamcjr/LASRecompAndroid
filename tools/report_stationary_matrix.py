#!/usr/bin/env python3
"""Export only completed, measured FPS data to the bilingual README; keep telemetry private."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
START = '<!-- stationary-matrix-s23:start -->'
END = '<!-- stationary-matrix-s23:end -->'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', type=Path)
    parser.add_argument('--write', action='store_true', help='Update both READMEs and sanitized summary JSON')
    parser.add_argument('--turnip-only', action='store_true', help='Export a completed Turnip run when the stock-driver run was intentionally stopped')
    args = parser.parse_args()
    folder = args.results.resolve()
    if not args.turnip_only and not (folder / 'complete.json').is_file():
        parser.error('Matrix has not completed; do not present partial results as a completed comparison')
    manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
    if manifest['device'] != 'SM-S911B':
        parser.error('This README section is for the Galaxy S23; add a separate device section for other phones')
    rows = []
    drivers = {}
    selected = ['turnip'] if args.turnip_only else ['turnip', 'system']
    if args.turnip_only and len(manifest['profiles']) != 40:
        parser.error('Expected the complete 40-profile S23 Turnip run')
    for driver in selected:
        drivers[driver] = json.loads((folder / driver / 'driver.json').read_text())['actual']
    for profile in manifest['profiles']:
        row = dict(profile=profile)
        for driver in selected:
            path = folder / driver / f'{profile["id"]:02d}' / 'result.json'
            result = json.loads(path.read_text(encoding='utf-8'))
            if result['profile'] != profile or result['driver'] != driver:
                raise RuntimeError('Profile/driver mismatch: ' + str(path))
            if args.turnip_only and not result['valid']:
                raise RuntimeError('Turnip run includes an invalid measurement: ' + str(path))
            row[driver] = dict(valid=result['valid'], fps=result.get('fps') if result['valid'] else None,
                               covered_s=result.get('covered_s'), intervals=result.get('intervals'))
        rows.append(row)
    summary = dict(device=manifest['device'], android=manifest['android'], date=manifest['created'][:10],
                   build_sha256=manifest['test_apk_sha256'], adpf=manifest['adpf'],
                   memory_path=manifest['memory_path'], drivers=drivers,
                   settle_s=manifest['settle_s'], measure_s=manifest['measure_s'], rows=rows)
    blocks = {}
    for pt in [False, True]:
        if pt:
            prefix = ('## Desempenho medido — Galaxy S23\n\n'
                      '21/10/2026 · SM-S911B, Adreno 740, Android 16 · **FPS em Auto**. Cena parada em Mabe Village, '
                      'mesma posição/save, uma execução por driver e todas as 40 combinações por execução. '
                      f'Cada perfil teve {manifest["settle_s"]:g} s de acomodação e {manifest["measure_s"]:g} s de coleta. '
                      'Os valores são FPS médios de apresentação medidos pelo Android. Auto permite ao jogo escolher 30/60 FPS; '
                      'não desbloqueia 120 FPS.\n\n'
                      '**Condição de teste:** APK local de diagnóstico, Turnip v26.0.0 R8 versus driver padrão Qualcomm, '
                      'ADPF preservado e `LARECOMP_VK_NO_PLACED=1` nos dois drivers. Sem esse contorno temporário de memória, '
                      'o Turnip encerrou por falta de memória neste S23, inclusive após fechar outros apps. '
                      'Esses números não representam uma correção já publicada na release 0.3.8.\n\n'
                      '<details>\n<summary>Comparação completa de FPS — 80 amostras</summary>\n\n'
                      '| Resolução | Aspecto | Filtro | AA | Turnip R8 | Padrão Qualcomm |\n'
                      '| --- | --- | --- | --- | ---: | ---: |\n')
            suffix = ('\n</details>\n\n'
                      'Sem FSR1 = bilinear; AA ligado = SMAA. Dynamic varia a resolução durante a amostra. '
                      'O teste é uma exploração por configuração, com uma amostra por combinação; não demonstra estabilidade '
                      'em movimento nem ganho causal entre drivers. Ordem, temperatura e caches podem influenciar os resultados. '
                      '[Protocolo e telemetria](docs/ANDROID-STATIONARY-MATRIX.md) · '
                      '[Dados de FPS](docs/benchmarks/galaxy-s23-20261021.json).\n\n'
                      'POCO F7 Ultra: repetir o mesmo protocolo, acrescentando 2x (48 combinações por driver); resultados pendentes.\n')
        else:
            prefix = ('## Measured performance — Galaxy S23\n\n'
                      '2026-10-21 · SM-S911B, Adreno 740, Android 16 · **Auto FPS**. Stationary Mabe Village scene, '
                      'same position/save, one game run per driver and all 40 combinations per run. '
                      f'Each profile had {manifest["settle_s"]:g} s settling and {manifest["measure_s"]:g} s collection. '
                      'Values are average presentation FPS measured by Android. Auto lets the game choose 30/60 FPS; '
                      'it does not unlock 120 FPS.\n\n'
                      '**Test condition:** local diagnostic APK, Turnip v26.0.0 R8 versus the stock Qualcomm driver, '
                      'unchanged ADPF and `LARECOMP_VK_NO_PLACED=1` with both drivers. Without that temporary memory workaround, '
                      'Android killed Turnip for low memory on this S23, including after closing other apps. '
                      'These results do not represent a fix already shipped in release 0.3.8.\n\n'
                      '<details>\n<summary>Full FPS comparison — 80 samples</summary>\n\n'
                      '| Resolution | Aspect | Filter | AA | Turnip R8 | Stock Qualcomm |\n'
                      '| --- | --- | --- | --- | ---: | ---: |\n')
            suffix = ('\n</details>\n\n'
                      'Without FSR1 = bilinear; enabled AA = SMAA. Dynamic changes resolution during the sample. '
                      'This is a configuration sweep with one sample per combination; it does not establish moving-scene '
                      'stability or a causal driver improvement. Order, temperature and caches can affect results. '
                      '[Protocol and telemetry](docs/ANDROID-STATIONARY-MATRIX.md) · '
                      '[FPS data](docs/benchmarks/galaxy-s23-20261021.json).\n\n'
                      'POCO F7 Ultra: repeat the same protocol, adding 2x (48 combinations per driver); results pending.\n')
        table = []
        for row in rows:
            p = row['profile']
            values = []
            for driver in selected:
                fps = row[driver]['fps']
                value = f'{fps:.2f}' if fps is not None else ('Coleta inválida' if pt else 'Invalid capture')
                values.append(value.replace('.', ',') if pt and fps is not None else value)
            aa = p['aa'] if p['aa'] != 'Off' or not pt else 'Desligado'
            table.append(f'| {p["resolution"]} | {p["aspect"]} | {p["upscaler"]} | {aa} | ' + ' | '.join(values) + ' |')
        if args.turnip_only:
            replacements = {
                'mesma posição/save, uma execução por driver e todas as 40 combinações por execução.':
                    'mesma posição/save, uma execução com Turnip e todas as 40 combinações.',
                'same position/save, one game run per driver and all 40 combinations per run.':
                    'same position/save, one Turnip game run with all 40 combinations.',
                'Turnip v26.0.0 R8 versus driver padrão Qualcomm,': 'Turnip v26.0.0 R8,',
                'Turnip v26.0.0 R8 versus the stock Qualcomm driver,': 'Turnip v26.0.0 R8,',
                'nos dois drivers': 'com Turnip', 'with both drivers': 'with Turnip',
                'Comparação completa de FPS — 80 amostras': 'FPS com Turnip — 40 amostras',
                'Full FPS comparison — 80 samples': 'Turnip FPS — 40 samples',
                ' | Padrão Qualcomm |': ' |', ' | Stock Qualcomm |': ' |',
                '| --- | --- | --- | --- | ---: | ---: |': '| --- | --- | --- | --- | ---: |',
                'nem ganho causal entre drivers': 'nem ganho causal de uma opção isolada',
                'or a causal driver improvement': 'or a causal improvement from an individual setting',
                'POCO F7 Ultra: repetir o mesmo protocolo, acrescentando 2x (48 combinações por driver); resultados pendentes.':
                    'Driver padrão: comparação interrompida por renderização incompleta (iluminação, grama e textos relatados). '
                    'Seus números parciais foram excluídos desta tabela. O teste do POCO F7 Ultra fica para depois da investigação de compatibilidade.',
                'POCO F7 Ultra: repeat the same protocol, adding 2x (48 combinations per driver); results pending.':
                    'Stock driver: comparison stopped because of incomplete rendering (reported lighting, grass and text issues). '
                    'Its partial FPS results are excluded from this table. POCO F7 Ultra testing is deferred while compatibility is investigated.',
            }
            for before, after in replacements.items():
                prefix = prefix.replace(before, after)
                suffix = suffix.replace(before, after)
        blocks[pt] = START + '\n' + prefix + '\n'.join(table) + '\n' + suffix + END
    if args.write:
        export = ROOT / 'docs/benchmarks/galaxy-s23-20261021.json'
        export.parent.mkdir(parents=True, exist_ok=True)
        export.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
        for pt, name in [(False, 'README.md'), (True, 'README.pt-BR.md')]:
            path = ROOT / name
            text = path.read_text(encoding='utf-8')
            if START in text:
                a, b = text.index(START), text.index(END) + len(END)
                text = text[:a] + blocks[pt] + text[b:]
            else:
                heading = '## Importar e exportar saves' if pt else '## Importing and exporting saves'
                if heading not in text:
                    raise RuntimeError('README insertion point missing')
                text = text.replace(heading, blocks[pt] + '\n\n' + heading, 1)
            path.write_text(text, encoding='utf-8')
        print(f'Exported {len(rows)} profiles across {len(selected)} driver(s) to both READMEs; raw telemetry remains local.')
    else:
        print(blocks[False])


if __name__ == '__main__':
    main()
