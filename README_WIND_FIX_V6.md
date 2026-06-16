# Wind Fix V6 — Dynamic Harmonic/Noise Model

Questa revisione implementa i sette interventi concordati per ridurre il cosiddetto effetto "vento" nelle modalità a bassa latenza, senza aumentare la latenza dichiarata alla DAW.

## Obiettivi

- preservare l'aria naturale delle note lunghe;
- evitare che rumore, respiro o bleed vengano trattati come una fondamentale affidabile;
- stabilizzare il percorso residuo nelle finestre da 128 e 256 campioni;
- ridurre l'autorità della correzione quando il segnale è noise-dominant o polifonico;
- mantenere Quality come riferimento sonoro, modificandola il meno possibile;
- rendere le regressioni misurabili e ripetibili.

## I sette interventi

### 1. Profili distinti per latenza

Experimental, Live e Quality non condividono più gli stessi tempi e soglie del modello armonico. Ogni modalità possiede valori separati per smoothing della maschera, persistenza del residuo, rilevamento di polifonia, affidabilità spettrale e riduzione massima del noise path.

### 2. Maschera armonica guidata da F0

Quando una FFT corta non risolve chiaramente i parziali, il motore costruisce una maschera parametrica attorno a `n * F0`. La larghezza dei lobi dipende dalla modalità, dalla confidence e dall'affidabilità spettrale. Le osservazioni spettrali reali restano prioritarie; la maschera F0-guided interviene come regolarizzatore, non come sostituto cieco.

### 3. Persistenza espressa in millisecondi

Attack e release del noise path vengono convertiti in coefficienti a partire dalla durata reale del frame. Il comportamento resta quindi coerente fra 128, 256 e 512 campioni e non dipende da un numero fisso di frame.

### 4. Stabilità temporale della maschera

La maschera armonica ha limiti separati di salita e discesa per frame, smoothing asimmetrico e un meter `maskStability`. Questo impedisce aperture e chiusure veloci del percorso residuo che possono produrre un soffio modulato.

### 5. Autorità wet adattiva

L'autorità della correzione è modulata da:

- consensus del tracker;
- affidabilità spettrale;
- stima di polifonia/bleed;
- voicing;
- stato di transizione;
- predominanza del noise path.

I frame puliti mantengono quasi tutta l'autorità. Quando il noise supera circa l'80%, il sistema non applica un gate rigido: sulle note lunghe e stabili conserva una quota di aria; sui frame poco affidabili riduce il wet e ritorna verso il dry allineato.

### 6. Polyphony/Bleed detector

Una seconda famiglia tonale concorrente, un accordo o un bleed strumentale aumentano `polyphony` e riducono `spectralReliability`. Il detector è volutamente prudente: non tenta di separare sorgenti, ma impedisce al correttore di diventare aggressivo quando l'ipotesi monofonica non è credibile.

### 7. Suite di regressione

Sono inclusi:

- latenza esatta per tutte le modalità;
- indipendenza dal block size;
- fondamentale debole con seconda armonica dominante;
- rumore bianco e input noise-dominant;
- polifonia sintetica;
- nota lunga ariosa;
- stabilità della maschera;
- output finito e limitato;
- harness per file audio reali;
- confronto CSV fra baseline e nuova versione;
- smoke test per AddressSanitizer/UndefinedBehaviorSanitizer.

## Nuovi meter

`ModernPitchEngine::Metering` espone anche:

- `polyphony`;
- `spectralReliability`;
- `maskStability`;
- `sustainedNoteSeconds`.

La GUI li mostra in forma compatta nella riga diagnostica.

## Integrazione

Sostituire la cartella `Source` con quella del pacchetto oppure applicare `WindFixV6_git.patch` dalla root del progetto:

```bash
git switch -c feature/wind-fix-v6
git apply --check WindFixV6_git.patch
git apply WindFixV6_git.patch
```

Per abilitare i test, aggiungere in fondo al `CMakeLists.txt` principale:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/Tests/AutotuneTests.cmake)
```

Poi:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DAUTOTUNE_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Il real-world runner si usa così:

```bash
AutotuneRealWorldRegression metrics.csv voce1.wav voce2.wav
python Tests/compare_regression_metrics.py baseline.csv metrics.csv
```

## Latenza

La revisione non modifica il ritardo fisso:

- Experimental: 128 campioni;
- Live: 256 campioni;
- Quality: 512 campioni.

Non introduce look-ahead e non richiede variazioni a `setLatencySamples()`.

## Limitazioni

- Il core DSP è stato compilato e testato con warning severi e smoke test ASan/UBSan; in questo ambiente non è stato costruito l'intero VST3 contro la tua installazione JUCE/Visual Studio.
- La polifonia è una stima di sicurezza, non una separazione di sorgenti.
- I profili sono una taratura iniziale basata sul corpus disponibile e vanno ancora provati su più cantanti, microfoni, stanze e livelli di gain.
- `perdersi export.zip` non è estraibile: è una porzione di archivio multi-volume/troncato. Occorre un singolo ZIP completo o i WAV originali.
