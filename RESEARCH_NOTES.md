# Notas técnicas finais — OpenXR Toolkit PSVR2 v1.0

## Eye Action lifecycle

A correção funcional `7cde664cf201f586ecac1349d69faed9f9000933` evita attach artificial antecipado sob OpenComposite. O Eye ActionSet é acrescentado à chamada real de `xrAttachSessionActionSets`; o Toolkit espera attach e `xrSyncActions` bem-sucedidos antes de consultar gaze. Apps OpenXR nativos sem ActionSets mantêm o fallback artificial. Eye Tracking e ETFR foram validados com Gunman Contracts e COMPOUND Demo.

Os logs `[PSVR2-DIAG]` permanecem enxutos: extensão, criação, attach, primeiro sync/readiness, transições relevantes, falhas XR e fim de sessão. Não há amostragem periódica de frame, sync ou projected gaze.

## Crop Resolution to FOV

A V1 (`463716b8725e3364e893f3ac1823590f5cbf2ab8`) implementou a recomendação linear. A V2 (`9d132cd63425831266b589b5cda7e7f4149822a5`) usa razões exatas por tangentes do FOV original e modificado de cada olho, com a mesma transformação de FOV Simple usada em `xrLocateViews`.

Como `xrEnumerateViewConfigurationViews` geralmente precede `xrLocateViews`, um cache miss usa fallback linear durante a primeira execução. O primeiro FOV original válido é salvo em `%LOCALAPPDATA%\OpenXR-Toolkit\configs\fov_crop_calibration_*.txt`. Após reiniciar o jogo, um cache hit compatível ativa Exact. A identidade inclui runtime, headset/system, fabricante, view configuration, número de views e resolução bruta por olho; não inclui percentual de FOV. Arquivo ausente, inválido ou corrompido é ignorado e recalibrado. Alterar runtime/headset/view configuration impede reutilização incompatível.

O Toolkit apenas muda `recommendedImageRectWidth/Height`; não força `xrCreateSwapchain`, `imageRect` ou viewport. Crop Off mantém o fluxo original. Conflitos com FOV Advanced, FSR/NIS/CAS ou resolução manual desativam somente o Crop. `[FOV-CROP]` registra cache, modo, FOV, razões, dimensões e se a recomendação foi aceita ou ignorada quando determinável.

Em Gunman Contracts/PSVR2/SteamVR OpenXR com FOV 90%, a resolução observada mudou de 3400×3468 para 2756×2872, ou ~67,1% dos pixels originais (~32,9% de redução). A RC1 passou o teste final de hardware em Gunman e COMPOUND; não foi medida aceitação de Exact em COMPOUND. Vertigo 2 permaneceu flat via OpenComposite system-wide mesmo sem a layer, e a substituição per-game de `openvr_api.dll` falhou; isso não foi classificado como regressão do Toolkit nem corrigido na v1.0.

A v1.0 parte da RC1 `81f0d3cf6361a11ec27ba03d85589d90b08cbea3` e altera somente identificação, documentação e empacotamento. Consulte `STATUS.md`, `TEST_MATRIX.md` e `docs/PSVR2_V1_README.md`.
