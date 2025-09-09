
# FaunaFrontierGBA — Enhanced (con CI)
Progetto GBA originale. Compila localmente con `make` (devkitPro) **oppure** via GitHub Actions che costruisce la ROM automaticamente.

## Build via GitHub Actions (consigliata se non vuoi configurare nulla)
1. Crea un repository GitHub nuovo e carica TUTTI i file di questa cartella.
2. Vai nella tab **Actions**: partirà il workflow **GBA ROM Build**.
3. A esecuzione finita, apri i dettagli del job → sezione **Artifacts** → scarica `faunafrontier.gba`.

## Build locale (devkitPro)
- Installa devkitPro (devkitARM + libgba).
- Esegui: `make` → trovi `faunafrontier.gba`.

## Note
- Il gioco usa salvataggi **SRAM**.
- Tutto il codice è originale e sotto licenza MIT.
