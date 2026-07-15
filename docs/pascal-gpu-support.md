# Compatibilidad con GPUs Pascal

> ⚠️ **Última actualización:** 2026-07-09  
> **PR de referencia:** [#63](https://github.com/Jota-project/jota-transcriber/pull/63)

---

## Resumen

Este proyecto usa **CUDA 12.2** como versión pinned del toolkit NVIDIA. No debe actualizarse a CUDA 12.3 o superior mientras se usen GPUs con arquitectura **Pascal**.

---

## GPUs afectadas (arquitectura Pascal, compute capability 6.1)

| GPU | VRAM | Estado |
|---|---|---|
| GTX 1060 | 3GB / 6GB | ✅ Soportada |
| GTX 1050 Ti | 4GB | ✅ Soportada |
| Tesla P4 | 8GB | ✅ Soportada |
| Tesla P40 | 24GB | ✅ Soportada |
| Tesla P100 | 12GB / 16GB | ✅ Soportada |
| Quadro P4000 | 8GB | ✅ Soportada |
| Quadro P5000 | 16GB | ✅ Soportada |

---

## Por qué CUDA 12.2 y no 12.8

A partir de **CUDA 12.3**, NVIDIA incluyó soporte para *forward compatibility* — la capacidad de que un binario compilado para una arquitectura GPU más nueva se ejecute en una GPU más antigua. Esta feature requiere que la GPU física tenga compute capability **≥9.0** (Ampere o superior).

Las GPUs Pascal (compute 6.1) **no soportan** esta característica, y cuando `ggml-cuda` intenta inicializarse con un toolkit 12.3+, falla con:

```
ggml_cuda_init: failed to initialize CUDA:
forward compatibility was attempted on non supported HW
```

**CUDA 12.2** es la última versión del toolkit NVIDIA con soporte completo y estable para Pascal.

---

## Qué pasa si intentas actualizar CUDA

1. **Dependabot** está configurado para ignorar PRs de `nvidia/cuda`. No debería generarte trabajo extra.
2. Si actualizas manualmente a 12.3+, el contenedor **arrancará** pero Whisper **no usará la GPU** — volverá a CPU silenciosamente.
3. El log mostrará el error de `forward compatibility` y la inferencia será lenta.

---

## Cómo verificar que la GPU se está usando

```bash
# Durante una transcripción, mira la GPU:
docker exec jota-transcriber nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv,noheader

# Salida esperada (GPU activa):
# 1424 MiB, 100 %

# Salida si está en CPU (NO usar):
# 2 MiB, 0 %
```

---

## Por qué se usa `CMAKE_CUDA_ARCHITECTURES=61`

El Dockerfile compila `ggml-cuda` con `-DCMAKE_CUDA_ARCHITECTURES=61`, donde `61` = Pascal (GTX 1060).

Si en el futuro quieres soportar otra GPU, cambia esto:
- **60** = Fermi (viejo, no recomendado)
- **61** = Pascal (GTX 10xx, Tesla P-series)
- **70** = Volta (Titan V, Tesla V100)
- **75** = Turing (GTX 16xx, RTX 20xx)
- **80** = Ampere (RTX 30xx, A100, H100)
- **86** = Ada Lovelace (RTX 40xx)

---

## Contacto con NVIDIA sobre CUDA + Pascal

Si NVIDIA elimina CUDA 12.2 de sus containers oficiales, habría que evaluar:
1. Usar CUDA 11.x (última versión 11.8 con soporte Pascal completo)
2. Compilar ggml-cuda desde código fuente con toolchain de CUDA 11.x
3. Cambiar a GPU Ampere o superior

Consulta la [tabla de compatibilidad CUDA](https://docs.nvidia.com/deploy/cuda-compatibility/) antes de cualquier cambio.
