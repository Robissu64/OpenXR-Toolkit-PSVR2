# OpenXR Toolkit PSVR2 - RC1

Esta é uma build comunitária de teste para PSVR2, distinta da versão oficial do OpenXR Toolkit. Ela inclui Eye-Tracked Foveated Rendering (ETFR) com OpenComposite e a opção **Crop Resolution to FOV**.

## Instalação

1. Feche jogos VR e SteamVR. Remova a instalação oficial do OpenXR Toolkit, se presente, para não registrar duas camadas com o mesmo nome interno. Guarde o instalador oficial para uma eventual restauração.
2. Configure **SteamVR como runtime OpenXR** do PSVR2 e confirme que o eye tracking funciona no PSVR2Toolkit/SteamVR.
3. Extraia **todos** os arquivos do artifact para uma pasta permanente. Mantenha DLLs, JSON e a pasta `shaders` juntos.
4. Execute `Install-Layer.ps1` em PowerShell com privilégios de administrador. Mantenha a pasta no lugar após instalar; o manifesto aponta para a DLL nela.
5. Reinicie SteamVR e o jogo. No menu do Toolkit, ative **Eye tracking** e **Foveated rendering** na aba Performance. Para o crop, na aba Appearance, escolha **Field of view: Simple**, reduza **Adjustment** e ative **Crop Resolution to FOV: On**.

Na primeira execução com Crop On, a resolução usa um fallback linear e o Toolkit salva a calibração do FOV. **Feche e reinicie o jogo** para ativar o modo Exact. Alterações posteriores de Adjustment ou Crop também exigem reinício para mudar a resolução. Reduzir o FOV cria naturalmente bordas ou corte visível na imagem.

## Desinstalação

Feche jogos e SteamVR. Execute `Uninstall-Layer.ps1` como administrador na mesma pasta. Depois, se desejar, reinstale a versão oficial. O script remove o registro da camada; a pasta pode ser removida após a desinstalação.

## Diagnóstico rápido

- Menu não aparece: confira a instalação, o runtime OpenXR ativo e o arquivo `BUILD_COMMIT.txt`; mantenha a pasta extraída no lugar.
- Eye tracking/ETFR inativo: confira eye tracking no PSVR2Toolkit, **Eye tracking** e **Foveated rendering** no menu. Procure `[PSVR2-DIAG]` no log.
- Crop mostra **Calibration pending**: execute o jogo até que `calibration stored - restart for exact crop` apareça no log; feche e reinicie. O cache fica em `%LOCALAPPDATA%\OpenXR-Toolkit\configs\fov_crop_calibration_*.txt`.
- Crop mostra **Inactive**: confira conflitos com FSR/NIS/CAS, override manual de resolução, FOV Advanced ou FOV a 100%. O motivo aparece em `[FOV-CROP]`.
- Sem redução de pixels: alguns jogos ignoram a resolução recomendada. Confira `crop recommendation accepted/ignored` e o tamanho pedido em `xrCreateSwapchain` no log.

Log: `%LOCALAPPDATA%\OpenXR-Toolkit\logs\XR_APILAYER_MBUCCHIA_toolkit.log`. Esta RC ainda precisa de validação adicional em outros jogos, runtimes e layouts de swapchain.
