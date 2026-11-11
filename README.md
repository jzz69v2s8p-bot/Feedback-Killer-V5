# Feedback Guard V5 (AU / VST3)

Rückkopplungs-Unterdrückung als Plugin. Ersetzt `feedback_guard_2ch.py` und die GUI-Version samt Python und BlackHole.
Das Plugin läuft direkt in Reaper in der Effektkette **vor EverClean**.

* Eine Instanz verarbeitet 1–8 Kanäle, jeder Kanal wird **unabhängig** analysiert (eigene Filter, eigene Anzeige).
* **Null Latenz.** Die Analyse läuft seitlich, das Signal geht nur durch die Filter.
* Keine Datei-Zugriffe, keine Speicher-Allokation und keine Ausgaben im Audio-Thread.
* Suchbereich einstellbar von **20 Hz bis 22 kHz** (obere Grenze bei 44,1 kHz automatisch bei ca. 20,3 kHz).
* **Einmessen-Modus:** Filter vom Soundcheck bleiben dauerhaft stehen und werden **mit dem Reaper-Projekt gespeichert**.

## Neu in v2 (gilt weiterhin)

| | |
|---|---|
| Frequenzbereich | untere Grenze 20 Hz (vorher 80), obere Grenze 22 kHz (vorher 16 kHz). FFT jetzt 8192 Punkte (Raster 5,9 Hz), Filterbreite im Bass automatisch angepasst, damit sie auch bei 20–60 Hz treffen. |
| Betrieb: Live / Einmessen | Einmessen = empfindlicher, schneller, neue Filter werden **fixiert** (orange). Live = Filter geben sich nach der Haltezeit wieder frei (rot). |
| Filter fixieren | Taste, die alle gerade aktiven Filter dauerhaft festhält (z. B. nach einer Probe). |
| Projekt-Speicher | Fixierte Filter stehen nach dem erneuten Öffnen des Projekts wieder da. Auch nach Samplerate-Wechsel. |
| Stimm-Schutz | Deutlich verbesserte Erkennung von Stimm-Grundtönen und Obertonreihen, strengere Frequenzstabilität. Im Test 0 Fehlalarme auf 4 × 60 s Sprache, auch im vollen Bereich 20 Hz–22 kHz. |

## Neu in v3 (gilt weiterhin)

| | |
|---|---|
| Nähe-Anzeige | Balken pro Kanal: wie nah ein Kanal an der Rückkopplung ist (grün → gelb → rot). Warnt **vor** dem Pfeifen. |
| Weiches Bypass | „Aktiv“ und die Kanal-Schalter blenden in 15 ms über, kein Knacken. |
| Pro-Kanal-Einstellungen | Kanalname (Doppelklick auf den Namen), Kanal an/aus, Empfindlichkeits-Versatz ±30 % je Kanal. Alles wird im Projekt gespeichert. |
| Raum-Presets | „Raum speichern… / laden…“: fixierte Filter als Datei (`.fgroom`), z. B. pro Saal. Laden ersetzt nicht, sondern ergänzt. |
| Liste exportieren | Textdatei mit Frequenz, Tonname und nächstem 31-Band-GEQ-Band (für den GEQ auf den M32-Hauptbussen) plus Absenkung. |
| PA-Referenz (optional) | Zusätzlicher Sidechain-Eingang: das PA-Signal (z. B. Pult-Mix). Nur Töne, die auch dort stark auftreten, gelten als Rückkopplung. Fehlt das Signal, arbeitet das Plugin normal. |
| Diagnose | Schalter „Diagnose aufzeichnen“: schreibt Ereignisse, Pegel und Spektren nach `~/Documents/FeedbackGuardV5/diagnose-….jsonl`, damit man Fehlalarme im Nachhinein untersuchen kann. |
| Prüfungen im Bau | GitHub-Workflow führt zusätzlich `auval`, `pluginval` und den Prozessor-Test aus (blockieren den Bau nicht). Extra-Workflows: Windows-VST3 und signierter/notarisierter Mac-Release (Letzterer ungetestet, braucht Apple-Developer-Konto und Secrets, siehe Kommentar in der Datei). |

**PA-Referenz einrichten (VST3 empfohlen):** In Reaper den Track auf mehr Kanäle stellen (z. B. 4), Kanal 1/2 = Mikrofone,
Kanal 3/4 = PA-Referenz per Send auf diese Kanäle routen, im Plugin-Routing die Eingänge 3/4 auf den Sidechain legen,
dann „PA-Referenz“ einschalten. Ob Reaper den Sidechain bei der AU-Version genauso anbietet, ist **nicht geprüft**.
Der Status steht unten rechts (aktiv / PA still / kein Signal).

## Neu in V5

* **Wandernde Töne bündeln** (Haken oben im Fenster, standardmäßig an): Pfeift ein Ton, der in der Frequenz springt (z. B. Mikrofon direkt vor dem Lautsprecher), setzt das Plugin nicht Filter um Filter, sondern **verbreitert ein frisches Filter** (Neu-Erkennung innerhalb ca. 18 %, maximal 30 % Spanne, nur wenn das Filter in den letzten 20 s aktiv war, und **nur ab 4 kHz**). Grund: Die erste Praxis-Aufnahme mit V5 zeigte, dass Bündeln bei Stimmfrequenzen (250 bis 1300 Hz) breite Filter auf Sprache setzt. Darunter bleibt es bei schmalen Filtern. Im Protokoll steht dann „Filter verbreitert auf a–b Hz“.
* Preis dafür: Ein breites Filter dämpft weniger tief (maximal 15 dB, im Test an den Sprungfrequenzen mindestens ca. 7 dB gegenüber ca. 20 dB bei schmalen Filtern). Dafür bleiben Filter frei. Der Haken lässt sich jederzeit ausschalten.
* Verbreiterte Filter werden im Projekt/Preset nur mit ihrer Mittenfrequenz und Tiefe gespeichert, nicht mit der Breite (nach dem Laden sind sie wieder schmal). Das ist eine bekannte Einschränkung.
* Getestet: Simulation mit fünf springenden Tönen (10,2 bis 12,1 kHz): ohne Bündeln 5 Filter, mit Bündeln 1 Filter. **Nicht** getestet: Verhalten in einem echten Raum.

## Änderungen nach den ersten Praxis-Aufnahmen (MacBook-Mikro + Lautsprecher)

* Aufgrund der Diagnose-Dateien: Filter werden erst ab −62 dBFS gesetzt (leise Töne lösten vorher viele Fehlalarme aus), die Notbremse ebenso.
* Wenn alle Filter belegt sind, wird der **unwichtigste** verdrängt (wenig Treffer, geringe Überhöhung, lange her), nicht mehr einfach der älteste. Ein oft bestätigter starker Ton behält so seinen Filter.
* Neue Warnung für Dauertöne außerhalb des Suchbereichs; Standard der oberen Grenze jetzt 16 kHz.
* Nicht gelöst: Ein Mikrofon direkt vor Lautsprechern (Laptop-Aufbau) ist ein extrem enger Regelkreis. Dort sind Pfeiftöne sehr laut und springen in der Frequenz. Das Plugin fängt sie meist nach ca. 1–2 s, aber nicht verzögerungsfrei. Für einen sauberen Test Kopfhörer oder getrennte Räume nutzen.

## 1. Installation

### Weg A: Fertig bauen lassen bei GitHub (kein Xcode nötig)

1. Kostenloses Konto auf github.com, neues **privates** Repository anlegen.
2. Den Inhalt dieses Ordners hochladen (*Add file → Upload files*).
3. **Wichtig, sonst fehlt die Aktion „Build macOS“:** Der Ordner `.github` beginnt mit einem Punkt, ist deshalb im
   Finder unsichtbar und wird beim Hochladen im Browser meist **nicht mitgenommen**. Ohne ihn gibt es auf GitHub
   keine Aktion. So legst du die Datei direkt auf GitHub an:
   * Im Repository *Add file → Create new file*.
   * In das Namensfeld genau diesen Pfad tippen: `.github/workflows/build-macos.yml`
     (beim Tippen von `/` entstehen die Ordner automatisch).
   * Den kompletten Inhalt der mitgelieferten, sichtbaren Datei **`WORKFLOW-zum-Einfuegen.yml`** hineinkopieren,
     unten *Commit changes* klicken.
4. Reiter **Actions** öffnen. Falls GitHub fragt, ob Workflows aktiviert werden sollen: bestätigen
   („I understand my workflows, go ahead and enable them“).
5. Links **Build macOS (AU + VST3)** wählen, rechts **Run workflow**. Nach ca. 10 Minuten steht unter dem Lauf
   ganz unten bei *Artifacts* der Download **FeedbackGuard-macOS**.
6. ZIP entpacken. `Feedback Guard V5.component` nach `~/Library/Audio/Plug-Ins/Components/`,
   `Feedback Guard V5.vst3` nach `~/Library/Audio/Plug-Ins/VST3/` kopieren.
7. Im Terminal einmal die Download-Sperre entfernen:
   ```bash
   xattr -cr ~/Library/Audio/Plug-Ins/Components/"Feedback Guard V5.component" ~/Library/Audio/Plug-Ins/VST3/"Feedback Guard V5.vst3"
   ```

Der Workflow läuft auf `macos-15` (Apple-Silicon-Runner). Ältere Labels wie `macos-13` und `macos-14` hat GitHub
abgeschaltet; falls auch `macos-15` einmal abgekündigt wird, in der Datei `runs-on:` auf `macos-latest` ändern.

Der Workflow führt vor dem Bau den Engine-Test aus und zeigt das Ergebnis an. Ein Testfehler stoppt den Bau nicht.

**Wenn der Lauf rot ist:** Auf den roten Lauf klicken, links den Eintrag **build** wählen und den Schritt mit dem roten
Kreuz aufklappen. Die letzten 30–40 Zeilen davon sind die eigentliche Fehlermeldung (die Zeile
„build-macos.yml#2 … pushed by …“ ist nur der Titel des Laufs).

### Weg B: Selbst bauen auf dem Mac

```bash
xcode-select --install          # einmalig, falls noch nicht vorhanden
brew install cmake              # einmalig
cd FeedbackGuard
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j 8
```

Beim ersten Mal lädt CMake JUCE 8.0.6 herunter (Internet nötig). Das Plugin wird automatisch nach
`~/Library/Audio/Plug-Ins/Components/` (AU) und `~/Library/Audio/Plug-Ins/VST3/` kopiert.

### Prüfen

```bash
auval -v aufx Fbg5 Fgrd         # muss mit "AU VALIDATION SUCCEEDED" enden
```

In Reaper: *Einstellungen → Plug-ins → AU → Re-scan* (bzw. VST → Clear cache/re-scan). Das Plugin heißt **Feedback Guard V5**.

## 2. Einbindung in Reaper (mit EverClean)

Python und BlackHole entfallen. Die M32 ist in Reaper Ein- **und** Ausgabegerät (dasselbe Gerät, damit gibt es keinen
Takt-Drift zwischen Ein- und Ausgang).

1. *Einstellungen → Audio → Device:* Eingang und Ausgang beide **M32**, 48 kHz, Puffer **128** (bei Aussetzern 256).
   Die Option *„Close audio device when stopped and application is inactive“* ausschalten, sonst bricht der Ton ab,
   wenn Reaper nicht im Vordergrund ist. *„Allow live FX multiprocessing“* einschalten.
2. Track 1: Eingang **M32 USB In 1** (Handmics), FX-Kette: **Feedback Guard V5 → EverClean**, Ausgang **M32 USB Out 1**.
3. Track 2: Eingang **M32 USB In 2** (Headsets), gleiche Kette, Ausgang **M32 USB Out 2**.
4. Beide Tracks: Aufnahme-bereit schalten und *Monitor input* wählen, damit das Signal live durchläuft.
5. Pultseitig bleibt das Routing wie bisher (Busse → USB Out, USB In → Rückweg-Kanäle).

### Empfehlungen zur Kette

* **Guard immer vor EverClean.** Der Guard soll das Signal sehen, bevor irgendetwas es verändert. Ein
  Rauschunterdrücker kann einen stehenden Ton verändern oder dämpfen und damit die Erkennung verfälschen. (Ich habe
  EverClean nicht geprüft; das ist eine allgemeine Überlegung für Rauschunterdrückung/Dereverb, bitte am Pult kurz bestätigen.)
* **Latenz des Gesamtwegs prüfen.** Der Guard fügt nichts hinzu, EverClean unter Umständen schon. Miss den Weg
  Pult → Reaper → Pult einmal mit einem Klick-Signal. Faustregel für Handmics mit Monitoring: möglichst unter 10–15 ms.
  Die Verzögerung steht im Loop, sie ändert die Frequenzen, an denen Feedback auftritt, aber nicht die Erkennung.
* **Eine Spur-Vorlage speichern** (*Track template*: Guard + EverClean). Dann ist jedes neue Projekt in Sekunden fertig.
* **Einstellungen pro Spur:** Handmics und Headsets verhalten sich unterschiedlich. Headsets (Mund nah, geringeres
  Feedback-Risiko) vertragen höhere *Haltezeit* und niedrigere *Empfindlichkeit*, Handmics eher umgekehrt.
* **Notfall-Taste:** Den Parameter *Aktiv* in Reaper per *Param → Learn* auf einen Controller-/MIDI-Taster legen.
  *Aktiv* aus = Signal läuft unverändert durch. Auf der M32 bleibt der Pult-Bypass (Original-Mikrokanäle direkt auf LR)
  der sicherste Weg, weil er unabhängig von Reaper funktioniert.
* **Stereo-Track mit zwei Quellen:** Eine Instanz verarbeitet beide Seiten getrennt (links Handmics, rechts Headsets),
  jede Seite hat ihre eigene Anzeige und eigene Filter.

## 3. Ablauf am Konzerttag

1. **Soundcheck:** Betrieb auf **Einmessen** stellen. Mikrofon wie später benutzen und die Verstärkung vorsichtig
   erhöhen, bis es anfängt zu pfeifen. Das Plugin setzt Filter (orange = fixiert) und das Pfeifen verschwindet.
   Schrittweise weiter aufdrehen, bis ihr den Pegel erreicht habt, den ihr braucht.
2. **Projekt speichern.** Die fixierten Filter sind jetzt Teil des Reaper-Projekts.
3. Betrieb auf **Live** stellen. Neue, unerwartete Filter (rot) geben sich nach der Haltezeit wieder frei. Wer sie
   behalten will, drückt **Filter fixieren**.
4. Beim ersten Einsatz mit einer neuen Stimme oder einem neuen Raum zuerst **Nur beobachten** einschalten und normal
   sprechen/singen lassen. Meldet das Protokoll etwas, obwohl kein Feedback war: *Empfindlichkeit* senken oder
   *Reaktion* auf *Vorsichtig* stellen.

## 4. Bedienung

Oben: **Aktiv** (aus = Signal läuft unverändert durch), **Nur beobachten** (erkennt und zeigt an, filtert aber nicht),
**Notbremse** (kurze Pegelabsenkung bei extrem schnellem Aufschaukeln), **Betrieb** (Live / Einmessen),
**Filter fixieren**, **Filter zurücksetzen** (entfernt auch fixierte Filter).

Jeder Kanal zeigt Spektrum (20 Hz–22 kHz), die gesetzten Filter (rot = automatisch, orange = fixiert, mit Frequenz und
Absenkung), eine Alarm-LED, Pegel und Notbremse. Unten steht ein Protokoll mit Uhrzeit, Kanal, Frequenz und Art der Erkennung.

| Regler | Bedeutung | Standard |
|---|---|---|
| Empfindlichkeit | Wie deutlich ein Ton aus dem Spektrum ragen muss. Höher = reagiert früher, mehr Fehlalarm-Risiko. | 50 % |
| Max. Absenkung | Tiefste Absenkung pro Filter. Ein neues Filter startet bei 6–12 dB und wird bei erneutem Aufschaukeln schrittweise vertieft. | 18 dB |
| Haltezeit | Nach dieser Zeit ohne erneutes Feedback blendet sich ein **nicht fixierter** Filter über ca. 5 s aus. Ganz rechts (600 s) = dauerhaft. | 60 s |
| Max. Filter | Maximale Zahl gleichzeitiger Filter je Kanal. Sind alle belegt, wird zuerst der älteste nicht fixierte ersetzt. | 8 |
| Untere / Obere Grenze | Nur in diesem Frequenzbereich wird gesucht (20 Hz … 22 kHz). Steht ein starker Dauerton außerhalb, zeigt der Kanal „… Hz ausserhalb!“ (gelb), filtert ihn aber nicht. | 150 Hz / 16 kHz |
| Notbremse | Tiefe der kurzen Pegelabsenkung. | 6 dB |
| Reaktion | Vorsichtig / Normal / Schnell: wie lange ein Ton auffällig bleiben muss, bevor gehandelt wird. | Normal |

**Zum Suchbereich:** Die Standardgrenzen 150 Hz / 16 kHz sind bewusst konservativ. Praxis-Erfahrung: Laptop-Mikrofon mit Lautsprechern pfiff bei 10,4 kHz, eine Grenze bei 10 kHz hätte das verpasst. Unter ca. 150 Hz liegen die
Grundtöne männlicher Stimmen, darüber entsteht das meiste PA-Feedback. Die Grenzen auf 20 Hz / 22 kHz zu erweitern ist
sinnvoll für Bass, Kick, Instrumente und Hochfrequenz-Pfeifen, braucht aber mehr Vorsicht (siehe Grenzen).

## 5. Wie die Erkennung funktioniert

Die alte Python-Version hat jeden lautesten Peak als Feedback gewertet und Sprache dadurch dauernd gefiltert.
Der neue Ansatz verlangt, dass ein Ton mehrere unabhängige Merkmale erfüllt:

1. **Auflösung:** FFT mit 8192 Punkten, Frequenz per parabolischer Interpolation (Genauigkeit im Test ±0,5 Hz).
2. **Schmalbandig:** Der Peak muss deutlich über seiner Umgebung liegen.
3. **Frequenzstabil:** Feedback steht (Schwankung im Test 0,01–0,2 Bins), Stimm-Teiltöne wandern (0,5–1,5 Bins).
4. **Wächst allein:** Steigt der Pegel der Umgebung (eine Oktave um den Peak) mit, ist es Sprache.
5. **Kein Oberton:** Gehört der Peak zu einer Obertonreihe (Stimme, Instrument), wird er ignoriert.
6. **Tiefe Frequenzen:** Unter 600 Hz wird länger beobachtet, bevor gehandelt wird (Stimm-Grundtöne).

Drei Regeln lösen aus: *schnelles Aufschaukeln*, *wachsender Ton* und *stehender Ton*. Die Filter sind schmale
Absenkungen (Q = 30, im Bass automatisch breiter, ca. 4 Hz), die weich einblenden.

## 6. Ehrliche Einschätzung / Grenzen

**Was getestet wurde** (`Tests/test_engine.cpp`, synthetische Signale: Sprachnachbildung mit Formanten, Tonhöhen-Gleiten
und Zischlauten plus simulierte Rückkopplungsschleife mit Raumecho):

| Test | Ergebnis |
|---|---|
| 4 × 60 s Sprache (variabel und monoton, Suchbereich 150–10000 Hz und 20–22000 Hz) | 0 Fehlalarme |
| Feedback 3 bis 25 dB/s, 1–6 kHz, mit und ohne Sprache | in 0,3–0,8 s gefangen, Filter auf ±1 Hz, Feedback blieb unter −45 dBFS (Sprachspitzen liegen bei −16 dBFS) |
| Feedback bei 20 Hz, 40 Hz, 20 kHz, 22 kHz | gefangen (Bass nach ca. 3 s, weil sich ein 20-Hz-Ton langsam aufbaut), Filter auf ±3 Hz |
| Einmessen / Live | Einmessen hält Filter auch nach Ende des Feedbacks, Live gibt sie nach der Haltezeit frei |
| Fixierte Filter nach Samplerate-Wechsel | bleiben erhalten |
| Rechenlast | ca. 2,2 % eines Kerns pro Kanal (Linux-Server, Rauschen als Eingang) |
| Prozessor-Test (`Tests/ProcessorTest.cpp`, **echtes JUCE auf Linux gebaut und ausgeführt**) | Projekt speichern/laden (vor und nach prepare, Samplerate-Wechsel), Presets, Export, weiches Bypass ohne Sprünge, Diagnose-Datei (gültiges JSON), Sidechain-Bus, Rückkopplungsschleife → Filter → fixiert → nach Neuladen weiter aktiv, Editor wird erzeugt und gezeichnet |

**Was NICHT getestet wurde:**

* **Das Plugin in einem echten Raum mit echten Mikrofonen und Lautsprechern.** Das heißt: erst im Soundcheck mit
  *Nur beobachten* erproben, nicht im ersten Live-Einsatz.
* **Der Bau als AU/VST3 auf dem Mac, `auval` und der Lauf in Reaper.** Der Plugin-Code läuft im Linux-Test mit echtem
  JUCE, aber die Plugin-Hülle (AU/VST3-Wrapper), Reapers Speicher-Reihenfolge und der Sidechain in Reaper sind nie
  ausprobiert worden. Die Speicherung der fixierten Filter im Projekt ist deshalb der Teil, den du als Erstes prüfen
  solltest: Einmessen → Projekt speichern → Reaper
  schließen → wieder öffnen → stehen die orangen Filter wieder da?
* **EverClean** in der Kette.

**Bekannte Grenzen:**

* **Untere Grenze weit unter 150 Hz:** Wer sie auf 20–100 Hz stellt, sucht im Bereich der Stimm-Grundtöne. Im Härtetest
  (Grenze 20 Hz, Stimme mit starkem 25-ms-Raumecho) gab es **1 Fehlalarm in 40 s** auf einem Grundton. Dort nur mit
  *Nur beobachten* testen oder die Grenze erst anheben, wenn Sprechmikrofone betroffen sind.
* Bei sehr **tiefen Stimmen** liegen die Obertöne so dicht, dass Einzeltöne im Hochton nicht mehr von Obertönen zu
  unterscheiden sind. Dort wird vorsichtshalber *nicht* gefiltert; echtes Feedback wird erst in der nächsten
  Sprechpause erkannt.
* Ein **reiner Pfeifton** (Flöte, Pfeifen) wird als „stehender Ton“ gefiltert und nach der Haltezeit wieder freigegeben.
* Nach dem Setzen eines Filters kann das Feedback auf eine **Nachbarfrequenz** ausweichen. Das Plugin setzt dann dort ein
  weiteres Filter; bei sehr breiten Raumresonanzen kann das mehrere Filter hintereinander brauchen.
* Das Plugin sitzt im Signalweg. Fällt Reaper oder der Mac aus, ist der Kanal weg. Pult-Bypass vorbereiten.
* Die Nähe-Anzeige und die PA-Referenz sind nur mit synthetischen Signalen getestet; die Schwellen brauchen echte Räume.
* Unsigniert (ad-hoc). Auf fremden Rechnern verlangt macOS ggf. eine Freigabe unter
  *Systemeinstellungen → Datenschutz & Sicherheit*.

## 7. Dateien

```
CMakeLists.txt               Projekt (lädt JUCE 8.0.6 automatisch)
Source/FeedbackEngine.h      Erkennung und Filter (reines C++, ohne JUCE)
Source/PluginProcessor.*     Plugin, Parameter, Kanalverwaltung, Projekt-Speicher
Source/PluginEditor.*        Oberfläche
Tests/ProcessorTest.cpp      Prozessor-Test (cmake -DFG_BUILD_PROCESSOR_TEST=ON)
Tests/test_engine.cpp        Engine-Test:  g++ -O2 -std=c++17 -ISource Tests/test_engine.cpp -o fg_test && ./fg_test
.github/workflows/           build-macos (Bau + Tests + auval + pluginval), build-windows, release-macos-signed (ungetestet)
WORKFLOW-zum-Einfuegen.yml   sichtbare Kopie der Workflow-Datei (für den Fall, dass .github nicht mit hochgeladen wurde)
```

JUCE steht unter AGPLv3 bzw. kommerzieller Lizenz. Für den privaten bzw. internen Einsatz ist das unproblematisch;
wer das Plugin weitergibt, muss den Quelltext mit weitergeben oder eine JUCE-Lizenz erwerben.
