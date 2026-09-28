# Notas técnicas — PSVR2 Toolkit RC1

## Eye Action lifecycle — resolvido

A correção `7cde664cf201f586ecac1349d69faed9f9000933` evita o attach artificial antecipado sob OpenComposite. O Eye ActionSet entra na chamada real de `xrAttachSessionActionSets`; o Toolkit só permite consulta de gaze depois de attach e `xrSyncActions` bem-sucedidos. A sessão nativa conserva o fallback para aplicações sem ActionSets. A fase 1 foi validada com ETFR em Gunman Contracts e COMPOUND Demo.

Na RC1, `[PSVR2-DIAG]` mantém extensão, criação, attach, primeiro sync/readiness, transições de atividade da pose, primeira localização válida, falhas distintas e fim de sessão. Amostras periódicas de frame, sync e projected gaze foram removidas para evitar spam em uso diário.

## Crop Resolution to FOV — V1 e V2 validados

V1 (`463716b8725e3364e893f3ac1823590f5cbf2ab8`) adicionou a opção Off/On e redução linear conservadora da recomendação. V2 (`9d132cd63425831266b589b5cda7e7f4149822a5`) passou a usar as razões exatas por tangentes do FOV original/modificado, compartilhando a transformação de FOV Simple usada em `xrLocateViews`.

`xrEnumerateViewConfigurationViews` pode ocorrer antes do primeiro `xrLocateViews`. Por isso, com cache miss, a execução usa fallback linear, captura o FOV original por olho, persiste em `%LOCALAPPDATA%\OpenXR-Toolkit\configs\fov_crop_calibration_*.txt` e pede reinício. O próximo início, com chave compatível, usa Exact. A chave contém runtime, sistema, fabricante, view configuration, número de views e recomendações brutas; percentual de FOV não faz parte da identidade. Cache inválido ou corrompido não é aplicado. O Toolkit não redimensiona swapchains da aplicação.

No Gunman Contracts em PSVR2 com FOV 90%, Exact recomendou e o app aceitou 2756×2872 em vez de 3400×3468: ~67,1% dos pixels originais e ~32,9% de redução estimada. Eye Tracking e ETFR seguiram ativos; nenhum `XR_ERROR` foi observado. A V2 no COMPOUND passou sanity check de abertura, sem medição de crop nesse jogo.

Limitações: FOV Advanced, FSR/NIS/CAS e resolução manual conflitam com Crop; algumas aplicações ignoram a recomendação; arrays/swapchains lado a lado podem escolher dimensões próprias. `[FOV-CROP]` registra modo, razões, dimensões e resultado `accepted`/`ignored`/`custom_or_undetermined` quando inferível. Medir desempenho exige teste em jogo.

## Release candidate

A branch `release/psvr2-toolkit-rc1` parte de V2 `9d132cd63425831266b589b5cda7e7f4149822a5` e limita-se a hardening, logs, status no menu, identificação visível, documentação e artifact de instalação. Consulte `STATUS.md`, `TEST_MATRIX.md` e `docs/PSVR2_RC1_README.md`.
