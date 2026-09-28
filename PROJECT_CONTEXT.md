# PSVR2 / OpenXR Toolkit — contexto do projeto

## Base da RC1

- Fork: https://github.com/Robissu64/OpenXR-Toolkit-PSVR2
- Branch: `release/psvr2-toolkit-rc1`, criada de `feature/crop-resolution-to-fov-v2` em `9d132cd63425831266b589b5cda7e7f4149822a5`.
- Eye Actions/OpenComposite: correção funcional em `7cde664cf201f586ecac1349d69faed9f9000933`; branch estável anterior `fix/opencomposite-eye-actions`.
- Crop V1: `463716b8725e3364e893f3ac1823590f5cbf2ab8`.
- Exact Crop V2: `9d132cd63425831266b589b5cda7e7f4149822a5`.
- RC1: hardening, logs de suporte, identificação visível, documentação e artifact de instalação. O commit final é identificado por `BUILD_COMMIT.txt`.

## Resultado validado

Eye-Tracked Foveated Rendering funciona no Gunman Contracts em OpenXR nativo e no COMPOUND Demo via OpenComposite. O lifecycle de Eye ActionSets foi resolvido: o Eye ActionSet entra no attach real, é sincronizado e só então consultado. O fallback artificial continua disponível para apps OpenXR nativos sem ActionSets.

No Gunman Contracts/PSVR2/SteamVR OpenXR, o Crop V2 com FOV Simple 90% teve cache hit, modo Exact e recomendação aceita: **3400×3468 → 2756×2872**, ou **67,1% dos pixels originais** (redução estimada de **32,9%**). Eye Tracking e ETFR continuaram funcionando, sem `XR_ERROR` observado. No COMPOUND/OpenComposite, a V2 foi instalada e o jogo abriu sem popup ou crash (sanity check); essa verificação não mede a redução de resolução nele.

## Funcionamento

O primeiro início com Crop On usa o fallback linear, observa o FOV original em `xrLocateViews` e grava a calibração. Depois de reiniciar o jogo, o Toolkit calcula a razão exata de tangentes por olho e entrega a nova recomendação em `xrEnumerateViewConfigurationViews`. A identidade do cache inclui runtime, sistema/headset, fabricante, view configuration, quantidade de views e resoluções brutas. Ele fica em `%LOCALAPPDATA%\OpenXR-Toolkit\configs\fov_crop_calibration_*.txt`. Cache inválido é ignorado e recalibrado.

Crop Off preserva o fluxo original. Crop On não força dimensões em `xrCreateSwapchain`; ganhos de pixels dependem de a aplicação aceitar a recomendação. FOV Advanced, FSR/NIS/CAS e override manual de resolução desativam somente o Crop, com motivo no log. Consulte `STATUS.md`, `TEST_MATRIX.md` e `docs/PSVR2_RC1_README.md`.
