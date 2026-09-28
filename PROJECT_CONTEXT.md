# OpenXR Toolkit PSVR2 v1.0 — contexto

## Versão final

- Fork: https://github.com/Robissu64/OpenXR-Toolkit-PSVR2
- Branch final: `release/v1.0`, criada de `release/psvr2-toolkit-rc1` no commit validado `81f0d3cf6361a11ec27ba03d85589d90b08cbea3`.
- Eye Action lifecycle funcional: `7cde664cf201f586ecac1349d69faed9f9000933`.
- Crop linear V1: `463716b8725e3364e893f3ac1823590f5cbf2ab8`.
- Exact Crop V2: `9d132cd63425831266b589b5cda7e7f4149822a5`.
- RC1 com hardening, logs enxutos e pacote de instalação: `81f0d3cf6361a11ec27ba03d85589d90b08cbea3`.
- O hash do commit final da v1.0 é gravado em `BUILD_COMMIT.txt` no artifact.

## Funcionalidades validadas

Eye Tracking do PSVR2 via OpenXR e por jogos OpenVR executados com OpenComposite, lifecycle corrigido dos Eye ActionSets e Eye-Tracked Foveated Rendering (ETFR) estão funcionais nos caminhos testados. Crop Resolution to FOV inclui fallback linear na primeira execução, calibração automática persistente e Exact Crop por razões de tangentes nas execuções seguintes. O cache separa runtime, headset/system, fabricante, view configuration, quantidade de views e recomendações brutas, sem incluir o percentual de FOV.

No PSVR2 com SteamVR/OpenXR, Gunman Contracts (OpenXR nativo) passou no teste final da RC1: abre normalmente, imagem normal, Eye Tracking, ETFR e Exact Crop com FOV 90%, sem regressão percebida. A medição anterior confirmou 3400×3468 original e 2756×2872 com Exact Crop, recomendação aceita: ~67,1% dos pixels originais, ou ~32,9% de redução de pixels recomendados. COMPOUND Demo via OpenComposite também passou no teste final de abertura e comportamento normal, sem crash; Eye Tracking e ETFR haviam sido validados nessa rota. A aceitação de Exact Crop em COMPOUND não foi medida.

Vertigo 2 não abriu corretamente via OpenComposite nos testes: a substituição per-game de `openvr_api.dll` não funcionou; com OpenComposite system-wide, abriu em modo flat. O comportamento persistiu sem a layer do Toolkit, portanto não foi identificado como regressão deste fork nem tratado como bug resolvido na v1.0.

## Operação e limites

Na primeira execução com Crop On, o cache é criado em `%LOCALAPPDATA%\OpenXR-Toolkit\configs\fov_crop_calibration_*.txt`; reiniciar o jogo ativa Exact. Alterar FOV/Crop também exige reinício para mudar resolução. Crop Off preserva o comportamento original. O Toolkit altera somente a recomendação de resolução; jogos podem ignorá-la. FOV Advanced e override manual de resolução desativam somente o Crop e registram o motivo. Com FSR/NIS/CAS, o Crop reduz primeiro a resolução e o upscaling aplica-se sobre o tamanho recortado. Consulte `STATUS.md`, `TEST_MATRIX.md` e `docs/PSVR2_V1_README.md`.
