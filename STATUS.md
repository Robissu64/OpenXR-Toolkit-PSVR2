# Status — OpenXR Toolkit PSVR2 RC1

## Concluído

- **Eye Tracking/OpenComposite: RESOLVIDO.** Attach e sync do Eye ActionSet na ordem correta, sem regressão observada no OpenXR nativo.
- **Crop Resolution to FOV V1: VALIDADO** como etapa funcional e instrumentada.
- **Exact Crop V2 e calibração persistente: VALIDADOS em hardware** no Gunman Contracts/PSVR2/SteamVR OpenXR.
- Gunman Contracts: Eye Tracking, ETFR e Exact Crop funcionam com FOV 90%; 3400×3468 original, 2756×2872 entregue e aceita (~67,1% dos pixels, ~32,9% de redução). Nenhum `XR_ERROR` observado.
- COMPOUND Demo/OpenComposite: Eye Tracking e ETFR validados na fase 1; V2 instalada, abre sem popup/crash no sanity check.

## Release candidate

Branch `release/psvr2-toolkit-rc1`, baseada em V2 `9d132cd63425831266b589b5cda7e7f4149822a5`. A RC1 reduz logs por frame, acrescenta estado Exact/Calibration pending no menu, protege a leitura do cache e produz o artifact `OpenXR-Toolkit-PSVR2-RC1-x64` com DLL, manifesto, dependências, shaders, scripts e documentação. `BUILD_COMMIT.txt` identifica a build.

## Limites conhecidos

- A aplicação pode ignorar `recommendedImageRectWidth/Height`; o Toolkit só informa `accepted`, `ignored` ou `custom_or_undetermined` no log.
- A primeira execução com Crop On usa fallback linear. Reinicie o jogo para aplicar Exact após salvar calibração; alterações de FOV/Crop também exigem reinício.
- FOV Advanced, upscalers FSR/NIS/CAS e override manual de resolução não são combinados com Crop.
- Recomendações são calculadas por olho. Apps com um único texture array podem escolher tamanho próprio; o log informa se ele comporta o máximo dos dois olhos.
- O sanity check da V2 no COMPOUND não prova aceitação da recomendação exata nem ganho de desempenho. A RC1 ainda precisa de teste final em hardware e em outros jogos.
