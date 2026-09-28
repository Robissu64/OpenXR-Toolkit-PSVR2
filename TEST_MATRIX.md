# Matriz de validação — OpenXR Toolkit PSVR2 v1.0

| Aplicação | Caminho | Eye Tracking e ETFR | Crop | Resultado |
| --- | --- | --- | --- | --- |
| Gunman Contracts | OpenXR nativo → SteamVR OpenXR → PSVR2 | Funcionais no teste final | Exact com FOV 90% funcional; 3400×3468 original → 2756×2872 entregue e aceita na medição anterior | **PASS**: abre, imagem normal, sem regressão percebida |
| COMPOUND Demo | OpenVR → OpenComposite → SteamVR OpenXR → PSVR2 | Funcionais na validação da fase Eye Actions | V2 instalada; aceitação de Exact Crop não medida neste jogo | **PASS**: abre, comportamento normal, sem crash |
| Vertigo 2 | OpenComposite per-game e system-wide | Não avaliado | Não avaliado | **Limitação externa não resolvida**: substituição per-game de `openvr_api.dll` falhou; system-wide abriu em flat mesmo sem a layer |

A razão observada `2756×2872 / (3400×3468)` é ~67,1%; a redução de ~32,9% se refere aos pixels recomendados, não a FPS medido. A RC1 `81f0d3cf6361a11ec27ba03d85589d90b08cbea3` passou o teste final de hardware em Gunman Contracts e COMPOUND Demo. Não extrapolar o resultado para jogos não testados.
