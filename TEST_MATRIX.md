# Matriz de validação — PSVR2 RC1

| Aplicação | Caminho | Eye Tracking / ETFR | Crop V2 | Estabilidade |
| --- | --- | --- | --- | --- |
| Gunman Contracts | OpenXR nativo → SteamVR OpenXR → PSVR2 | Funciona; gaze ativo e ETFR acompanha os olhos | FOV 90%, cache hit, Exact, recomendação aceita: 3400×3468 → 2756×2872; 67,1% dos pixels originais | Sem `XR_ERROR` observado |
| COMPOUND Demo | OpenVR → OpenComposite → SteamVR OpenXR → PSVR2 | Funciona desde a correção de Eye Actions; attach e sync ordenados | V2 instalada; sanity check de abertura, sem medição de resolução nesta etapa | Abre sem popup/crash |

O valor 67,1% é `2756×2872 / (3400×3468)`; a redução de 32,9% refere-se à quantidade de pixels recomendada, não a FPS medido. A validação acima é do código V2 `9d132cd63425831266b589b5cda7e7f4149822a5`. A RC1 adiciona hardening e empacotamento; aguarda verificação final em hardware.

Para analisar outro jogo, compare `[FOV-CROP]` (cache, modo, dimensões entregues, `xrCreateSwapchain`, aceitação) e `[PSVR2-DIAG]` (criação/attach/sync/readiness, falhas). Apps que ignoram a recomendação podem não economizar pixels.
