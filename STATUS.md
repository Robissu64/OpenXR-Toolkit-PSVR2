# Status — OpenXR Toolkit PSVR2 v1.0

**Concluído e aprovado em hardware.** Branch final `release/v1.0`, baseada na RC1 validada `81f0d3cf6361a11ec27ba03d85589d90b08cbea3`.

- Eye Tracking do PSVR2 via OpenXR e OpenComposite: funcional nos jogos testados.
- Lifecycle dos Eye ActionSets: corrigido; attach real e sync precedem consultas de gaze sob OpenComposite.
- Eye-Tracked Foveated Rendering: funcional em Gunman Contracts e COMPOUND Demo.
- Crop Resolution to FOV: funcional. Primeira execução usa fallback linear e salva calibração; execuções seguintes usam Exact Crop por tangentes com cache persistente por runtime/headset/view configuration.
- Gunman Contracts / OpenXR nativo / SteamVR OpenXR / PSVR2: **PASS** no teste final da RC1; abre, imagem normal, Eye Tracking, ETFR e Exact Crop com FOV 90%, sem regressão percebida. Resultado observado: 3400×3468 → 2756×2872, ~67,1% dos pixels originais e ~32,9% de redução recomendada.
- COMPOUND Demo / OpenComposite: **PASS** no teste final da RC1; abre normalmente, sem crash, comportamento normal. Eye Tracking e ETFR foram validados nessa rota. Aceitação de Exact Crop nesse jogo não foi medida.

Artifact final: `OpenXR-Toolkit-PSVR2-v1.0-x64`, com DLL, manifesto, dependências, shaders, scripts, README, licença e `BUILD_COMMIT.txt`. O workflow mantém MSVC v142, submódulos recursivos, Git LFS e pull do Omnicept LFS.

**Limites conhecidos:** a aplicação pode ignorar a resolução recomendada; a primeira calibração e mudanças de FOV/Crop exigem reinício; FOV Advanced, FSR/NIS/CAS e resolução manual não são combinados com Crop. Vertigo 2 abriu em flat com OpenComposite system-wide mesmo sem a layer, e a substituição per-game de `openvr_api.dll` não funcionou; não foi identificado como regressão do Toolkit nem corrigido nesta versão. Outros jogos e runtimes não foram validados.
