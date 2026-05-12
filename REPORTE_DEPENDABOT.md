# Reporte Dependabot — jota-transcriber

## PRs ya mergeadas ✅

| # | Dependencia | Cambio | Estado |
|---|-------------|--------|--------|
| #13 | `actions/checkout` | v4 → v6 | ✅ MERGED |
| #12 | `actions/upload-artifact` | v4 → v7 | ✅ MERGED |
| #26 | `actions/setup-node` | v4 → v6 | ✅ MERGED |

---

## PRs abiertas pendientes

### PR #25 — whisper.cpp (submódulo)
- **De:** `19ceec8` → `9386f23` (v1.8.2 → v1.8.4)
- **Archivos:** `third_party/whisper.cpp`
- **Estado:** ABI compatible — solo cambios internos de whisper.cpp y ajustes a GGML
- **Riesgo:** Bajo. No hay cambios en la API pública de whisper.
- **Recomendación:** ✅ Mergear — trae fix de UTF-8 en segment wrapping y mejoras internas de rendimiento.

---

### PR #30 — nvidia/cuda Docker image
- **De:** CUDA 12.8.0-runtime-ubuntu22.04 → **13.2.1-runtime-ubuntu22.04**
- **Archivos:** `Dockerfile` (builder + runtime stages)
- **Tu GPU:** NVIDIA GeForce GTX 1060 3GB (Pascal, CC 6.1)
- **Driver actual:** 570.211.01

#### Análisis de compatibilidad

**Tu driver 570 soporta CUDA 12.x máximo (no 13.x).**

La imagen CUDA 13.2.1 requiere un driver que soporte CUDA 13+. El branch R570 solo soporta hasta CUDA 12.x.

El branch R580 (último que soporta GPUs Pascal/Maxwell/Volta) soporta CUDA 13.x.

#### Opciones:

| Opción | Driver | CUDA support | Disponible en tu sistema |
|--------|--------|-------------|--------------------------|
| Mantener 570 | 570.211.01 | CUDA 12.x | ✅ Actual |
| Actualizar a 580 | 580.142 | CUDA 13.x | ❌ No instalado |
| Bajar imagen | 12.8.0 | CUDA 12.x | ✅ Compatible |

**Recomendación:** Actualizar driver a `nvidia-driver-580` y luego mergear PR #30.

#### Pasos para actualizar driver:
```bash
sudo apt update
sudo apt install nvidia-driver-580
sudo reboot
```

---

## Resumen

| Prioridad | Acción |
|-----------|--------|
| 🔴 Alta | Actualizar driver a 580 + mergear PR #30 (necesita reinicio) |
| 🟡 Media | Mergear PR #25 (whisper.cpp — bajo riesgo) |
| 🟢 Hecho | PRs #13, #12, #26 (actions) — ya mergeadas |

---

## Notas

- El branch 580 es el **último** que soporta tu GTX 1060 (Pascal). A partir del 590 NVIDIA deja de dar soporte a Pascal.
- La PR #30 necesita driver 580 para funcionar correctamente con CUDA 13.2.1.
