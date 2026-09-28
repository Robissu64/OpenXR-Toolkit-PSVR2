# Próximos passos após RC1

1. Instalar o artifact `OpenXR-Toolkit-PSVR2-RC1-x64` conforme `docs/PSVR2_RC1_README.md` e confirmar `BUILD_COMMIT.txt`.
2. Fazer smoke test final da RC1 em Gunman Contracts (OpenXR nativo) e COMPOUND Demo (OpenComposite), verificando estabilidade, ETFR, estado da calibração e logs essenciais.
3. Em outros jogos, verificar se a recomendação foi aceita e medir FPS/frametime separadamente; redução de pixels não garante ganho proporcional de desempenho.
4. Registrar incompatibilidades específicas de aplicação, runtime e layout antes de propor qualquer nova feature.

A base funcional de Exact Crop é `9d132cd63425831266b589b5cda7e7f4149822a5`. Eye Actions funcionais vieram de `7cde664cf201f586ecac1349d69faed9f9000933`. Não alterar algoritmo ou lifecycle durante a finalização. Manter toolset v142, submódulos recursivos, Git LFS e Omnicept LFS no workflow.
