# Validazione Wind Fix V6

## Stato

La revisione è stata validata sul core DSP e su estratti reali delle due voci precedentemente fornite. I risultati non sono una valutazione percettiva definitiva, ma misure di regressione utili per verificare che la stabilizzazione non comprometta la correzione.

## Test funzionali superati

- impulso in uscita esattamente a 128, 256 e 512 campioni;
- output indipendente dal block size DAW;
- tono pulito rilevato entro 2 Hz;
- fondamentale debole mantenuta con seconda armonica dominante;
- rumore bianco non forzato nel pitch shifter;
- affidabilità bassa su input noise-dominant;
- meter di polifonia più alto con due famiglie concorrenti;
- affidabilità ridotta in presenza di polifonia;
- età della nota espressa in secondi;
- stabilità della maschera superiore a 0,92 sul test sintetico;
- quota residua preservata su nota lunga ariosa;
- nessun NaN/Inf e output limitato nelle tre modalità.

Un smoke test separato con AddressSanitizer e UndefinedBehaviorSanitizer è terminato con exit code 0.

## Risultati su voce reale

Le metriche vengono calcolate nelle regioni vocali affidabili, confrontando la versione precedente con V6.

### `voce.mp3`

| Modalità | Riduzione della modulazione del noise path | Wet mediano precedente → V6 | Riduzione della modulazione high-band |
|---|---:|---:|---:|
| Experimental | 60,9% | 0,798 → 0,750 | 11,6% |
| Live | 34,8% | 0,796 → 0,773 | 10,1% |
| Quality | 13,1% | 0,796 → 0,773 | 6,1% |

### `vox old scattered fantasies.mp3`

| Modalità | Riduzione della modulazione del noise path | Wet mediano precedente → V6 |
|---|---:|---:|
| Experimental | 76,6% | 0,788 → 0,768 |
| Live | 49,9% | 0,784 → 0,779 |
| Quality | 18,3% | 0,786 → 0,777 |

Sulla seconda voce, l'età mediana della nota in Experimental è circa 0,49 s e il percentile 90 circa 1,11 s: il modello entra quindi realmente nella regione di preservazione dell'aria sulle note sostenute.

## Interpretazione

La diminuzione più forte avviene in Experimental e Live, dove le finestre brevi causavano la maggiore instabilità. Quality cambia meno e rimane il riferimento. Il wet mediano scende in modo moderato, non drastico: il sistema non risolve il problema semplicemente bypassando la correzione.

## Materiale non utilizzabile

`perdersi export.zip` dichiara di appartenere a un archivio multi-volume e contiene offset/dimensioni oltre la fine del file ricevuto. Il test CRC fallisce già sul primo WAV. Non è stato quindi usato per le metriche.

## Prossimi test in DAW

Verificare in particolare:

- frasi sussurrate e vocal fry;
- note lunghe con molto room noise;
- consonanti sibilanti seguite da vocali;
- bleed di chitarra o cuffie nel microfono;
- cambi rapidi fra registro pieno e falsetto;
- automazione Amount e Speed;
- buffer 32/64/128;
- confronto A/B a loudness pareggiato fra baseline, V6 e Quality.
