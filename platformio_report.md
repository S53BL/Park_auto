# Incident Report — PlatformIO Build Fix Session

## Projekt: Avtomatizacija Pokritega Parkirišča (ESP32-S3)

### Datum: 2026-05-16 | Platform: pioarduino 55.03.38 | Framework: Arduino + ESP-IDF 5.5.4

---

## Povzetek

Celotna seja je bila namenjena popravilu zlomljene `pio run` kompilacije za ESP32-S3 projekt
z `pioarduino` platformo verzija 55.03.38 in frameworkom Arduino/ESP-IDF 5.5.4.
Vsaka odpravljena napaka je razkrila naslednjo — skupaj je bilo odpravljenih **9 ločenih vzrokov
za neuspešen build**. Trajna rešitev je pre-build skripta `scripts/fix_build_env.py`.

---

## Incident 1 — MissingPackageManifestError

### Simptom

```text
MissingPackageManifestError
```

Napaka takoj ob zagonu `pio run`, preden se sploh začne kompilacija.

### Vzrok

PlatformIO `"stable"` tag za pioarduino platformo je bil premaknjen na novejšo verzijo
paketa, ki nima `package.json`. PlatformIO 6.1.19 to interpretira kot neveljaven paket.

### Rešitev

`platformio.ini` zaklenjen na specifično verzijo:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38/platform-espressif32.zip
```

Namesto `platform = https://github.com/pioarduino/platform-espressif32.git#stable`

### Prizadete datoteke

- `platformio.ini` — sprememba `platform =` vrstice

---

## Incident 2 — BOM korupcija v `.piopm`

### Simptom

```text
UnicodeDecodeError / JSONDecodeError
```

PlatformIO ne more prebrati `framework-arduinoespressif32-libs/.piopm`.

### Vzrok

Datoteka `.piopm` je imela UTF-8 BOM (`\xef\xbb\xbf`) na začetku, ki ga Python JSON
parser zavrne kot neveljaven JSON.

### Rešitev

Datoteka `.piopm` je bila prepisana brez BOM-a (čisti UTF-8).
Trajna rešitev: `fix_piopm_bom()` v `scripts/fix_build_env.py` (Fix 2).

### Prizadete datoteke

- `C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\.piopm`

---

## Incident 3 — `ARDUINO_EVENT_RUNNING_CORE` redefined [-Werror]

### Simptom

```text
<command-line>: error: "ARDUINO_EVENT_RUNNING_CORE" redefined [-Werror]
```

Napaka v fazi **"Compile Arduino IDF libs"** (wpa_supplicant komponenta), takoj po začetku
IDF lib kompilacije. Manifest: ~50 vzporednih napak istega tipa.

### Vzrok

`platformio.ini` je imel `-DARDUINO_EVENT_RUNNING_CORE=0` v `build_flags`. Ta define
se je "prelil" v kompilacijo IDF komponent (faza `call_compile_libs()`), kjer framework
že interno definira `ARDUINO_EVENT_RUNNING_CORE`. Duplikat povzroči `-Werror` fatal.

**Mehanizem preliva:** Ko `android.py` pokliče `SConscript("espidf.py")` za IDF lib
kompilacijo, SCons environment podeduje vse `build_flags` vključno z user-definiranimi
`-D` makri.

### Rešitev

Odstranil `-DARDUINO_EVENT_RUNNING_CORE=0` iz `build_flags`. Dodan komentar:

```ini
; NE DODAJ -DARDUINO_EVENT_RUNNING_CORE tu — framework to ze definira interno.
;   CONFIG_ASYNC_TCP_RUNNING_CORE=0 v custom_sdkconfig zadostuje.
```

### Prizadete datoteke

- `platformio.ini` — odstranjeno iz `build_flags`
- `.pio/build/` — zbrisano za čist rebuild

---

## Incident 4 — esp-dsp komponenta: broken symbolic link (Windows)

### Simptom

```text
CMake Error: Path ...managed_components/espressif__esp-dsp/applications/
azure_board_apps/apps/3d_graphics/3d_graphics.gif does not exist
or is a broken symbolic link
```

Napaka v CMake fazi med IDF lib kompilacijo.

### Vzrok

Tarball `espressif/esp-dsp 1.8.0` na Espressif komponentnem registru vsebuje
**symbolic linke** (`azure_board_apps/apps` → `graphics`). Python-ov `tarfile.extractall()`
na Windows teh sym-linkov ne ustvari — nadomestne poti so prazne. CMake nato preveri
CHECKSUMS.json (489 vpisov) in znajde manjkajoče datoteke.

**Podrobnost idf-component-manager logike:**

- `dependency_up_to_date()` → `validate_hash_eq_hashfile()` — bere `.component_hash`
- Brez `.component_hash`: sproži re-download tarball-a → isti problem znova
- Z `.component_hash`: komponenta je sprejeta kot-je, CHECKSUMS.json se ne preverja

### Rešitev

Ustvarjena datoteka `.component_hash` z hash vrednostjo iz `dependencies.lock`:

```text
12e44db246517a627bc0fe2b255b8335410c0215c98bfba2df5a8e3edea839ef
```

Pot: `managed_components/espressif__esp-dsp/.component_hash`
Vsebina: 64-znakovni hex string, UTF-8 brez BOM, brez newline.

**Zakaj je varno:** `espressif__esp-dsp/CMakeLists.txt` ne vključuje `azure_board_apps/`
v build sistem — manjkajoče GIF datoteke niso potrebne za kompilacijo.

Trajna rešitev: `fix_esp_dsp_symlink()` v `scripts/fix_build_env.py` (Fix 1).

### Prizadete datoteke

- `managed_components/espressif__esp-dsp/.component_hash` — USTVARJENO

---

## Incident 5 — Partition table: 16 MB ne gre v 8 MB flash

### Simptom

```text
Partitions tables occupies 16.0MB of flash (16777216 bytes)
which does not fit in configured flash size 8MB.
```

Napaka v fazi validacije particijske tabele med IDF lib kompilacijo.

### Vzrok

ESP32-S3 board `esp32-s3-devkitc-1` ima privzeto `upload.flash_size = 8MB` v board JSON.
`espidf.py` bere flash velikost za IDF sdkconfig iz `board_upload.flash_size`, **ne** iz
`board_build.flash_size`. V `platformio.ini` je bila nastavljena samo:

```ini
board_build.flash_size = 16MB   ; to espidf.py ne bere!
```

`gen_esp32part.py` (partition validator) je prav tako klican z `board.get("upload.flash_size", "4MB")`.

**Lokacija v kodi:** `espidf.py` vrstica ~410, `gen_esp32part.py` klic vrstica ~2755

### Rešitev

Dodano v `platformio.ini`:

```ini
board_upload.flash_size = 16MB
```

### Prizadete datoteke

- `platformio.ini` — dodano `board_upload.flash_size = 16MB`

---

## Incident 6 — `undefined reference to '__wrap_log_printf'`

### Simptom

```text
ld.exe: .pio/build/parking_esp32s3/libad0/WiFi/STA.cpp.o:
undefined reference to `__wrap_log_printf'
```

Linker napaka, zadnji korak pred ustvarjanjem firmware.elf.

### Vzrok

`framework-arduinoespressif32-libs/esp32s3/flags/ld_flags` vsebuje:

```text
-Wl,--wrap=log_printf
```

Ta linker flag povzroči, da se vsak klic `log_printf` (npr. `log_e(...)`, `log_w(...)` makri
v `STA.cpp`) preusmeri na `__wrap_log_printf`. **Nobena precompilirana knjižnica** v paketu
niti nobena komponenta `framework-arduinoespressif32` ne definira `__wrap_log_printf`.

**Tehnična razlaga GNU ld `--wrap`:**

- Vsak `U log_printf` (undefined ref) → postane `U __wrap_log_printf`
- `T log_printf` v `libframework-arduinoespressif32.a` → dostopna kot `__real_log_printf`
- `__wrap_log_printf` mora biti definiran — ni nikjer → linker error

**Preveritev:**

- `nm libframework-arduinoespressif32.a | grep __wrap` → samo `__wrap_esp_panic_handler`
- `esp32-hal-log-wrapper.c` → definira `__wrap_esp_log_write`, `__wrap_esp_log_writev` — NE `__wrap_log_printf`
- Vsi `.a` fajli v `esp32s3/lib/`, `opi_opi/`, itd. → 0 zadetkov za `__wrap_log_printf`
- IDF `components/log/src/*.c` → samo `esp_log_printf`, ne `log_printf`

To je **bug v pioarduino 55.03.38** — `ld_flags` vsebuje wrap zahtevo brez implementacije.

### Rešitev

Ustvarjena datoteka `src/log_printf_wrap.c` v projektu:

```c
#include <stdarg.h>

extern int log_printfv(const char *format, va_list arg);

int __wrap_log_printf(const char *format, ...) {
    va_list arg;
    va_start(arg, format);
    int len = log_printfv(format, arg);
    va_end(arg);
    return len;
}
```

`log_printfv` je definiran v `esp32-hal-uart.c` (vrstica 1654), kliče `vsnprintf` + `ets_printf`
brez povratnega klica na `log_printf` — brez rekurzije.

### Prizadete datoteke

- `src/log_printf_wrap.c` — USTVARJENO

---

## Dodaten pomožni ukrep — `flag_any_custom_sdkconfig` debug

Med odpravljanjem Incidenta 6 je bila napačno zbrisana napačna datoteka `sdkconfig`.

**Pričakovano:** `C:\...\framework-arduinoespressif32-libs\esp32s3\sdkconfig`

**Dejansko mesto za `flag_any_custom_sdkconfig`:** `C:\...\framework-arduinoespressif32-libs\sdkconfig` (root paketa)

`flag_any_custom_sdkconfig` v `arduino.py` (vrstica 314):

```python
flag_any_custom_sdkconfig = (FRAMEWORK_LIB_DIR is not None and
                            exists(str(Path(FRAMEWORK_LIB_DIR) / "sdkconfig")))
```

`FRAMEWORK_LIB_DIR` = `platform.get_package_dir("framework-arduinoespressif32-libs")` = **root paketa**, ne `esp32s3/` poddir.

---

## Incident 7 — `framework-arduinoespressif32-libs` nima `package.json`

### Simptom

```text
MissingPackageManifestError: Could not find one of 'package.json', '.piopm' in
C:\Users\..\.platformio\packages\framework-arduinoespressif32-libs
```

Napaka po uspešnem `fix_build_env.py`, v fazi ko PlatformIO preverja nameščene pakete.

### Vzrok

Tarball `esp32-core-3.3.8-libs.tar.xz` (s pioarduino GitHub releases) ne vsebuje `package.json`.
Ob ekstrakciji nastane samo `.piopm` (PlatformIO interni manifest). Novejše verzije PlatformIO
zahtevajo `package.json` za validacijo paketa. Ob ponovni namestitvi paketa ta problem nastopi samodejno.

### Kdaj nastopi

- Fresh klon repozitorija + `pio run`
- Ročno brisanje `framework-arduinoespressif32-libs` direktorija
- PlatformIO cache reset (`pio run --target clean` + brisanje paketa)

### Rešitev

Dodana funkcija `fix_libs_package_json()` v `scripts/fix_build_env.py` (Fix 4).
Skripta bere `name` in `version` iz `.piopm` in ustvari minimalen `package.json`:

```json
{
  "name": "framework-arduinoespressif32-libs",
  "version": "5.5.4+sha.735507283d",
  "description": "Arduino ESP32 precompiled libraries (pioarduino 55.03.38)"
}
```

### Prizadete datoteke

- `scripts/fix_build_env.py` — dodana funkcija `fix_libs_package_json()` (Fix 4)

---

## Incident 8 — Kaskadna destrukcija `esp32s3/` subdirektorija

### Simptom

```text
Error: Missing Arduino sdkconfig template at
'C:\Users\..\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\sdkconfig'
```

Napaka nastopi med IDF lib kompilacijo (`*** Compile Arduino IDF libs ***`).

### Vzrok — kaskada v `arduino.py`

Med odpravljanjem Incidenta 6 je bila **ročno zbrisana napačna datoteka** `esp32s3/sdkconfig`
(namesto root `sdkconfig`). To je sprožilo naslednjo kaskado v `arduino.py`:

```text
1. matching_custom_sdkconfig()
   bere esp32s3/sdkconfig — datoteka manjka
   vrne False (mismatch)

2. check_reinstall_frwrk() → True
   ker matching_custom_sdkconfig() == False

3. safe_remove_sdkconfig_files()
   pobrišee root sdkconfig iz framework-arduinoespressif32-libs/

4. safe_framework_cleanup()
   POBRIŠEE CELOTEN esp32s3/ SUBDIREKTORIJ

5. call_compile_libs()
   poskusi začeti IDF recompile
   NAPAKA: "Missing Arduino sdkconfig template at esp32s3/sdkconfig"
   esp32s3/sdkconfig je pogoj za začetek kompilacije — a je bil ravnokar pobrisan
```

### Ključna ugotovitev

`esp32s3/sdkconfig` je **template datoteka** (ne generirano). PlatformIO jo potrebuje
kot predlogo za custom sdkconfig. Brez nje ne more niti začeti IDF kompilacije.
`matching_custom_sdkconfig()` jo primerja s projektnim `custom_sdkconfig` — kakršnakoli
razlika (vključno z manjkajočo datoteko) sproži rebuild kaskado.

### Pravilo

**NIKOLI ne brišite:**

```text
C:\Users\..\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\sdkconfig
```

To je sistemska datoteka PlatformIO. Če jo zbrišete, pobrišete `esp32s3/` in build propade.

### Rešitev

Ker `esp32s3/` ni mogoče obnoviti selektivno (ni ločen paket, je del tarbala),
je treba pobrisati **cel paket** in ga pustiti PlatformIO da ga prenese znova:

```powershell
Remove-Item "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs" -Recurse -Force
pio run   # prenese paket + IDF recompile (~14 min)
```

Dodana zaščita: `check_esp32s3_subdir()` v `scripts/fix_build_env.py` (Fix 5) preveri
ob vsakem buildu ali `esp32s3/` obstaja in izpiše jasno opozorilo z recovery ukazom.

### Prizadete datoteke

- `C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\` — POBRISANO in OBNOVLJENO
- `scripts/fix_build_env.py` — dodana funkcija `check_esp32s3_subdir()` (Fix 5)

---

## Incident 9 — PlatformIO samodejno zamenjal pioarduino z uradnim frameworkom

### Simptom

```text
HARDWARE: ESP32S3 240MHz, 320KB RAM, 8MB Flash   (pricakovano: 8MB PSRAM, 16MB Flash)
framework-arduinoespressif32 @ 3.3.8             (pricakovano: pioarduino)
```

Plošča bootloopa ali se obnese nepravilno. AsyncTCP/WiFi problemi.

### Vzrok

Ko PlatformIO ne najde pioarduino ZIP-a v lokalnem cache-u (po `--target clean`,
ali na fresh klonu brez omrežne dostopnosti do GitHub releases), včasih naloži
**uradni Espressif framework** `framework-arduinoespressif32 @ 3.3.8` kot fallback.

Posledice napačnega frameworka:

- Nima podpore za OPI PSRAM na Waveshare ESP32-S3-Touch-LCD-3.5B
- Povzroči bootloop (OPI PSRAM init bug v bootloaderju < v3.x)
- Nima `__wrap_log_printf` implementacije (Incident 6 se ponovi)
- Ima napačen board profil (8MB Flash namesto 16MB, No PSRAM)

### Zaznava

```text
fix_build_env.py output:
  OPOZORILO: namesena platforma morda ni pioarduino!
  Najdeno: espressif32 @ ...
  Pricakovano: pioarduino @ 55.03.38
```

ali v PlatformIO output:

```text
framework-arduinoespressif32 @ 3.3.8
```

### Rešitev

```powershell
pio run --target clean
pio run
```

PlatformIO ob ponovnem zagonu prenese pioarduino iz URL-ja v `platformio.ini`.

### Prizadete datoteke

- Vzrok napisanja `scripts/fix_build_env.py` (Fix 3: `check_framework()`)

---

## Trajna rešitev — `scripts/fix_build_env.py`

### Namen

Pre-build skripta, ki se avtomatsko zažene pred **vsakim** `pio run` (kot `extra_scripts`).
Odpravlja vse znane fresh-klon in cache-reset probleme brez ročnega posredovanja.

### Integracija v build sistem

```ini
; platformio.ini
extra_scripts =
    pre:scripts/fix_build_env.py    ; MORA biti prva
    pre:scripts/compress_assets.py
```

### Kako deluje — 5 avtomatskih kontrol

#### Fix 1 — `fix_esp_dsp_symlink()` (Incident 4)

Preveri ali datoteka `managed_components/espressif__esp-dsp/.component_hash` obstaja
in vsebuje pravilno vrednost. Če ne, jo ustvari:

```text
12e44db246517a627bc0fe2b255b8335410c0215c98bfba2df5a8e3edea839ef
```

Ta hash `idf-component-manager` sprejme kot dokaz, da je komponenta veljavna,
in preskoči CHECKSUMS.json validacijo (ki bi odkrila manjkajoče symlink datoteke).

#### Fix 2 — `fix_piopm_bom()` (Incident 2)

Prebere `~/.platformio/packages/framework-arduinoespressif32-libs/.piopm` in
preveri za UTF-8 BOM (`\xef\xbb\xbf`). Če ga najde, prepiše datoteko brez BOM-a.
Pred prepisom validira da je JSON veljaven.

#### Fix 3 — `check_framework()` (Incident 9)

Prebere `~/.platformio/platforms/espressif32/platform.json` in preveri:

- Ali datoteka sploh obstaja (če ne: pioarduino ni nameščen)
- Ali je `version` = `55.03.38` ali `name` vsebuje `pioarduino`

Samo **opozori** — ne popravi samodejno (zahteva `pio run --target clean`).

#### Fix 4 — `fix_libs_package_json()` (Incident 7)

Preveri ali `~/.platformio/packages/framework-arduinoespressif32-libs/package.json` obstaja.
Če ne, ga ustvari z imenom in verzijo prebrano iz `.piopm`.
Izvede se samo kadar paket obstaja (`.piopm` je prisoten) a `package.json` manjka.

#### Fix 5 — `check_esp32s3_subdir()` (Incident 8)

Preveri ali `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/` obstaja
in ali vsebuje `sdkconfig`. Trije scenariji:

- Paket ne obstaja → normalno (PlatformIO bo prenesel)
- Paket obstaja, `esp32s3/` manjka → KRITIČNO opozorilo + recovery ukaz
- `esp32s3/sdkconfig` manjka → OPOZORILO pred IDF compile napako

### Pričakovan output ob normalnem buildu

```text
[fix_build_env] === Preverjanje build okolja ===
[fix_build_env] esp-dsp .component_hash: OK (12e44db246517a62...)
[fix_build_env] .piopm BOM: OK (ni BOM-a)
[fix_build_env] framework-arduinoespressif32-libs package.json: OK
[fix_build_env] esp32s3/sdkconfig: OK
[fix_build_env] framework: OK (pioarduino 55.03.38)
[fix_build_env] === Preverjanje koncano ===
```

---

## Recovery vodič — Kaj storiti ko build propade

### Scenarij A — Fresh klon na novem računalniku

```powershell
pio run   # bo trajal ~15-27 min prvic (download + IDF recompile)
```

Ob prvem buildu po klonu fix_build_env.py samodejno:

- ustvari `.component_hash` (Fix 1) — symlink problem odpravljen
- ustvari `package.json` (Fix 4) — MissingPackageManifestError odpravljen
- IDF recompile teče samodejno (~14 min) — normalno

Če build propade kljub temu, nadaljujte z Scenarijem B ali C.

---

### Scenarij B — `MissingPackageManifestError` ali `UnicodeDecodeError`

```text
Simptom:
  MissingPackageManifestError
  UnicodeDecodeError / JSONDecodeError
```

```powershell
# Preverite vsebino fix_build_env.py outputa v build logu
# Nato preverite package.json in .piopm:
ls "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\"

# Ce package.json manjka: fix_build_env.py bi moral to odpraviti samodejno.
# Preverite da je fix_build_env.py res PRVA skripta v extra_scripts!

# Ce .piopm manjka: cel paket je poskodovan — brisanje in ponoven prenos:
Remove-Item "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs" -Recurse -Force
pio run
```

---

### Scenarij C — `esp32s3/sdkconfig` kaskada (Incident 8)

```text
Simptom:
  Error: Missing Arduino sdkconfig template at '...\esp32s3\sdkconfig'
  ali:
  [fix_build_env] KRITICNO: esp32s3/ subdirektorij manjka!
```

`esp32s3/sdkconfig` je bil pobrisan (ročno ali s kaskado). `esp32s3/` ni mogoče
selektivno obnoviti — cel paket mora biti prenesen znova.

```powershell
# EDINA PRAVILNA RESITEV:
Remove-Item "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs" -Recurse -Force

# Preverite da je mapa res pobrisana:
Test-Path "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs"
# mora vrniti False

# Zazenite build — bo trajal ~14-27 min (download + IDF recompile):
pio run
```

Po uspešnem buildu preverite:

```text
[fix_build_env] esp32s3/sdkconfig: OK
*** Copied compiled esp32s3 IDF libraries ***
[SUCCESS]
```

**PRAVILO:** Nikoli ne brišite datotek v `framework-arduinoespressif32-libs\esp32s3\` ročno.

---

### Scenarij D — Napačen framework (pioarduino zamenjan z uradnim)

```text
Simptom:
  HARDWARE: ESP32S3 240MHz, 320KB RAM, 8MB Flash   (NAPACNO)
  framework-arduinoespressif32 @ 3.3.8             (NAPACNO)
  ali:
  [fix_build_env] OPOZORILO: namesena platforma morda ni pioarduino!
```

```powershell
# Resitev 1: clean + rebuild (pioarduino se prenese iz URL v platformio.ini)
pio run --target clean
pio run

# Ce to ne pomaga — preverite kateri platform je namesecen:
python -c "import json; d=json.load(open(r'C:\Users\S53BL\.platformio\platforms\espressif32\platform.json')); print(d.get('version'), d.get('name'))"
# Mora vsebovati: 55.03.38 ali pioarduino

# Ce je uradni framework:
Remove-Item "C:\Users\S53BL\.platformio\platforms\espressif32" -Recurse -Force
pio run   # prenese pioarduino iz URL v platformio.ini
```

---

### Scenarij E — esp-dsp CMake broken symbolic link

```text
Simptom:
  CMake Error: Path ...espressif__esp-dsp/... does not exist or is a broken symbolic link
```

```powershell
# fix_build_env.py bi moral to odpraviti samodejno (Fix 1).
# Preverite ali .component_hash obstaja:
Get-Content "managed_components\espressif__esp-dsp\.component_hash"
# Mora vsebovati: 12e44db246517a627bc0fe2b255b8335410c0215c98bfba2df5a8e3edea839ef

# Ce manjka ali napacen — rocna obnova:
python -c "open('managed_components/espressif__esp-dsp/.component_hash', 'wb').write(b'12e44db246517a627bc0fe2b255b8335410c0215c98bfba2df5a8e3edea839ef')"
pio run
```

---

### Scenarij F — Splošna diagnostika (ne veste kaj je narobe)

```powershell
# Preverite fix_build_env.py output v build logu — VSE vrstice morajo biti zelene:
#   esp-dsp .component_hash: OK
#   .piopm BOM: OK
#   package.json: OK
#   esp32s3/sdkconfig: OK
#   framework: OK (pioarduino 55.03.38)

# Preverite klucne datoteke:
Test-Path "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\sdkconfig"
Test-Path "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\package.json"
Test-Path "managed_components\espressif__esp-dsp\.component_hash"
Test-Path "src\log_printf_wrap.c"
# Vse morajo vrniti True

# HARDWARE vrstica "320KB RAM, 8MB Flash" je NORMALNA za board esp32-s3-devkitc-1
# To je display iz board JSON — ne dejanska konfiguracija.
# Dejanska konfiguracija (16MB Flash, OPI PSRAM) je v board_build.* in custom_sdkconfig.
# Potrditev pravilnega IDF recompile:
#   *** Compile Arduino IDF libs for parking_esp32s3 ***
#   *** Copied compiled esp32s3 IDF libraries to Arduino framework ***
```

---

---

## Incident 10 — Bootloop po PSRAM optimizaciji: direktno editiranje framework sdkconfig

### Datum: 2026-05-19 | Kontekst: poskus optimizacije SRAM → PSRAM

### Simptom

```text
Panic handler entered multiple times. Abort panic handling. Rebooting ...
ESP-ROM:esp32s3-20210327
st:0xc (RTC_SW_CPU_RST),boot:0x29 (SPI_FAST_FLASH_BOOT)
saved PC:0x40041a76
mode:DIO, clock div:1
load:0x3fce2820,len:0xfb0
load:0x403c8700,len:0xaf4
load:0x403cb700,len:0x2e34
entry 0x403c8898
Panic handler entered multiple times. Abort panic handling. Rebooting ...
```

Plošča se ne zažene. Crash se zgodi **takoj pri entry point 0x403c8898** — preden se sploh
zažene katera koli aplikacijska koda. `saved PC:0x40041a76` je ROM naslov (PSRAM init koda).
Ponavilja se v neskončni zanki.

### Vzrok

Med poskusom optimizacije SRAM porabe (prenos statičnih bufferjev v PSRAM) so bile
**direktno urejene framework sdkconfig datoteke** izven projekta:

```text
C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\sdkconfig
C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\sdkconfig
```

Dodani/spremenjeni vnosi:

```text
# DODANO (ni smelo biti):
CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y

# SPREMENJENO (iz "not set" v y):
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` povzroči da linker postavi BSS segment
v PSRAM (zewnetram). Med zagonom, ko ESP-IDF startup koda (ROM) poskuša inicializirati OPI PSRAM,
pride do napake v ROM kodi pri naslovu `0x40041a76`. Ker je BSS (vključno s podatki ki jih
potrebuje panic handler) v PSRAM, ki ni dostopen, pride do **double fault**:
panic handler sam sesuje → `"Panic handler entered multiple times"`.

**Ključni mehanizem:** `matching_custom_sdkconfig()` v `arduino.py` primerja
**hash inline `custom_sdkconfig`** iz `platformio.ini` s tistim shranjenIM v `sdkconfig.defaults`.
Direktno editiranje framework sdkconfig datotek ta hash mehanizem **obide** — PlatformIO
misli da je konfiguracija pravilna (hash se ujema), medtem ko so framework datoteke
nekonzistentne.

### Kaj je bilo narejeno (napačno)

1. Dodan `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` v obe framework sdkconfig datoteki
2. Dodan `CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` v obe framework sdkconfig datoteki
3. Dodan `EXT_RAM_BSS_ATTR` na statične buffere v kodi (potem reverted)
4. Spremenjen `custom_sdkconfig` v `platformio.ini` (potem reverted)
5. Framework dvakrat prevedel (enkrat z napačnimi nastavitvami, enkrat brez)

### Recovery postopek

**Korak 1:** Revertiraj projekt sdkconfig (če je bil spremenjen):

```powershell
cd "c:\PlatformIO\Projekti\Park_auto"
git checkout HEAD -- sdkconfig.parking_esp32s3
```

**Korak 2:** Zbriši `sdkconfig.defaults` — to prisili PIO v popoln framework reinstall + IDF recompile:

```powershell
Remove-Item "sdkconfig.defaults"
```

**Korak 3:** Poženi full rebuild (~30 min):

```powershell
pio run
```

PIO bo:

- Zaznal manjkajoč `sdkconfig.defaults` → `matching_custom_sdkconfig()` vrne `False`
- Sprožil `safe_framework_cleanup()` → `pm.install()` → `call_compile_libs()`
- Prenesel svež `framework-arduinoespressif32` + `framework-arduinoespressif32-libs`
- Prevedel IDF iz source z **samo** `custom_sdkconfig` iz `platformio.ini`

**Korak 4:** Po uspešnem buildu flash factory binary:

```powershell
& "C:\Users\S53BL\.platformio\penv\Scripts\python.exe" `
  "C:\Users\S53BL\.platformio\packages\tool-esptoolpy\esptool.py" `
  --port COM6 --chip esp32s3 --baud 921600 `
  write_flash 0x0 ".pio\build\parking_esp32s3\firmware.factory.bin"
```

### Pravila — kaj se NIKOLI ne sme

| Prepovedano | Razlog |
|---|---|
| Direktno editirati `framework-arduinoespressif32-libs/sdkconfig` | Obide PIO hash mehanizem, framework ostane nekonzistenten |
| Direktno editirati `framework-arduinoespressif32-libs/esp32s3/sdkconfig` | Ista nevarnost; dodatno: brisanje te datoteke sproži Incident 8 kaskado |
| Dodati `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` v `custom_sdkconfig` | BSS v PSRAM → ROM PSRAM init crash → bootloop |
| Dodati `EXT_RAM_BSS_ATTR` na kritične statične podatke | Brez `SPIRAM_ALLOW_BSS_SEG=y` ignoriran, z njim → bootloop |
| Spremeniti `custom_sdkconfig` brez premisleka | Vsaka sprememba hash-a sproži IDF recompile ~30 min (Incident 8 context) |

### Zaznava tega problema

```text
# Simptom 1: takojna bootloop (< 1s po napajanju)
# Simptom 2: saved PC v ROM območju (0x40000000–0x4005FFFF)
# Simptom 3: "Panic handler entered multiple times" (ne navaden panic z backtrace)
# Simptom 4: load segmenti so OK (bootloader naloži firmware), crash je v startup kodi

# Diagnostični test — preveri framework sdkconfig:
Select-String -Path "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\sdkconfig" `
  -Pattern "SPIRAM_ALLOW_BSS|ESP_ALLOW_BSS"
# Mora vrniti SAMO "not set" vrstice, NE "=y" vrstice
```

---

## Incident 10b — Dodatek: zakaj "reverting code" ne zadostuje

Ko je framework prevedel z napačnimi nastavitvami in nato revertirani na pravilne,
je hash v `sdkconfig.defaults` bil posodobljen na pravilno vrednost (LwIP-only). PlatformIO
je zato menil da je framework v redu — in ni sprožil novega IDF recompile.

**Posledica:** Framework `.a` datoteke so bile morda skompirane z mešanimi nastavitvami.
Samo brisanje `sdkconfig.defaults` prisili PIO da **vedno** naredi clean reinstall + recompile
ne glede na hash.

**Pravilo:** Po kakršni koli eksperimentalni spremembi `custom_sdkconfig` ki jo potem reverted:

```powershell
Remove-Item "sdkconfig.defaults"   # prisili clean rebuild
pio run                            # ~30 min
```

---

## Scenarij G — Bootloop: `Panic handler entered multiple times` takoj ob bootu

```text
Simptom:
  Panic handler entered multiple times. Abort panic handling. Rebooting ...
  saved PC: 0x4004xxxx  (ROM območje!)
  entry 0x403c8898 → takoj crash
  Ponavlja se v neskončni zanki brez kakršnega koli aplikacijskega outputa
```

**Vzrok je skoraj zagotovo v framework sdkconfig ali IDF kompilaciji**, ne v aplikacijski kodi.

```powershell
# KORAK 1: Diagnostika — preveri framework sdkconfig
Select-String -Path "C:\Users\S53BL\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\sdkconfig" `
  -Pattern "SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY|ESP_ALLOW_BSS_SEG"
# Ce najde "=y" vrstice → Incident 10; nadaljuj s Korakom 2

# KORAK 2: Revertiraj projekt sdkconfig
git checkout HEAD -- sdkconfig.parking_esp32s3

# KORAK 3: Prisili clean framework rebuild
Remove-Item "sdkconfig.defaults"

# KORAK 4: Rebuildi (~30 min)
pio run

# KORAK 5: Poglej ali se framework pravilno reinstallira:
# V build outputu mora biti:
#   *** Reinstall Arduino framework ***
#   Tool Manager: Installing ... esp32-core-3.3.8.zip
#   *** Compile Arduino IDF libs for parking_esp32s3 ***
#   *** Copied compiled esp32s3 IDF libraries ***
#   [SUCCESS]
```

**Ce problem ostane po clean rebuild:** Problem je v aplikacijski kodi (manj verjetno za ta tip
cracha). Preveri ali je katera od statičnih spremenljivk označena z `EXT_RAM_BSS_ATTR` —
to postavi podatke v PSRAM območje ki morda ni dostopno med initom.

---

## Končni rezultat

| Metrika | Vrednost |
|---|---|
| Čas build-a — 1. uspešen (samo kompilacija) | 337 sekund |
| Čas build-a — po obnovi paketa (download + IDF recompile) | 1631 sekund (~27 min) |
| Čas build-a — Incident 10 recovery (full reinstall + IDF recompile) | 1805 sekund (~30 min) |
| `firmware.elf` | 41 MB |
| `firmware.bin` | ~2.1 MB |
| `firmware.factory.bin` | ~2.1 MB |
| Exit code | 0 |

### Ustvarjene/spremenjene datoteke

| Datoteka | Akcija | Namen |
|---|---|---|
| `platformio.ini` | SPREMENJENA | Incidenti 1, 3, 5, 9; verzija 1.0.12-dev |
| `managed_components/espressif__esp-dsp/.component_hash` | USTVARJENA | Incident 4 |
| `src/log_printf_wrap.c` | USTVARJENA | Incident 6 |
| `scripts/fix_build_env.py` | USTVARJENA | Incidenti 2, 4, 7, 8, 9 (trajna rešitev) |
| `.gitignore` | SPREMENJENA | dovoli scripts/ v gitu |
| `C:\...\framework-arduinoespressif32-libs\.piopm` | POPRAVLJENO | Incident 2 |
| `C:\...\framework-arduinoespressif32-libs\` | POBRISANO IN OBNOVLJENO | Incident 8 |

### Priporočila za prihodnost

1. **Ob nadgradnji pioarduino platforme** — preveriti ali je `--wrap=log_printf` še vedno v `ld_flags`
   in ali je `__wrap_log_printf` sedaj definiran v paketu. Takrat `src/log_printf_wrap.c` ni več potreben.

2. **Ob nadgradnji `espressif/esp-dsp`** — posodobiti `ESP_DSP_COMPONENT_HASH` v `fix_build_env.py`
   z novo vrednostjo iz `managed_components/espressif__esp-dsp/dependencies.lock`.
   Preveriti ali nova verzija še vsebuje symlinke v tarbalu.

3. **Nikoli ne dodajati `-DARDUINO_EVENT_RUNNING_CORE` v `build_flags`** — framework to definira interno.

4. **Pri spremembi flash velikosti** vedno nastaviti OBE vrednosti:

   ```ini
   board_build.flash_size = 16MB
   board_upload.flash_size = 16MB   ; espidf.py bere to, ne board_build!
   ```

5. **Nikoli ne brisati ročno datotek v `framework-arduinoespressif32-libs\esp32s3\`** —
   brisanje `esp32s3/sdkconfig` sproži kaskado ki pobriše cel `esp32s3/` subdirektorij.
   Edina varna obnova: brisanje celega paketa + `pio run`.

6. **HARDWARE vrstica `320KB RAM, 8MB Flash` je normalna** za board `esp32-s3-devkitc-1`
   z pioarduino — to je display iz board JSON, ne dejanska konfiguracija. Dejanska konfiguracija
   (16MB Flash, OPI PSRAM) je določena z `board_build.*` in `custom_sdkconfig` v `platformio.ini`.
