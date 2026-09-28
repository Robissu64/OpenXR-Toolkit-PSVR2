# OpenXR Toolkit PSVR2 v1.0

Fork comunitário com Eye Tracking do PSVR2 via OpenXR, correção de compatibilidade com OpenComposite, Eye-Tracked Foveated Rendering e Crop Resolution to FOV com cálculo exato por tangentes e calibração persistente.

**Instalação:** Windows x64, PSVR2 com eye tracking funcional e SteamVR como runtime OpenXR. Extraia o pacote completo, execute `Install-Layer.ps1` como administrador e siga o `README.md` incluído. A primeira execução com Crop On calibra o FOV; reinicie o jogo para usar Exact. Use `Uninstall-Layer.ps1` para remover a camada antes de restaurar a instalação oficial.

**Validação:** Gunman Contracts/OpenXR nativo passou com Eye Tracking, ETFR e Exact Crop; com FOV 90%, a recomendação observada passou de 3400×3468 para 2756×2872 (~32,9% menos pixels recomendados). COMPOUND Demo/OpenComposite passou em abertura e funcionamento normal; Eye Tracking e ETFR foram validados nessa rota. Exact Crop não foi medido em COMPOUND.

**Limites:** alguns jogos ignoram a resolução recomendada. Vertigo 2 não funcionou corretamente via OpenComposite nos testes e permaneceu em flat sem a layer do Toolkit; não é uma correção incluída nesta versão. Outros jogos não foram validados.
