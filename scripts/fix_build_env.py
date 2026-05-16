#!/usr/bin/env python3
"""
fix_build_env.py — Pre-build skripta za Park_auto projekt
Datum: 2026-05

NAMEN:
  Odpravi znane build probleme ki se pojavijo na fresh klonu ali
  po resetiranju PlatformIO cache-a. Teče avtomatsko pred vsakim
  pio run kot extra_scripts = pre:scripts/fix_build_env.py.

PROBLEMI KI JIH REŠUJE:
  [1] esp-dsp symlink (Incident 4 iz platformio_report.md)
      espressif/esp-dsp 1.8.0 tarball vsebuje symbolic linke ki jih
      Python tarfile.extractall() na Windows ne ustvari pravilno.
      CMake nato vrže: "does not exist or is a broken symbolic link"
      Rešitev: ustvari .component_hash da idf-component-manager
      preskoči CHECKSUMS.json validacijo.

  [2] .piopm BOM korupcija (Incident 2 iz platformio_report.md)
      framework-arduinoespressif32-libs/.piopm ima UTF-8 BOM (\xef\xbb\xbf)
      ki ga Python JSON parser zavrne: UnicodeDecodeError / JSONDecodeError.
      Rešitev: preberi datoteko in jo prepiši brez BOM-a.

  [3] Napačen framework (nova napaka 2026-05-16)
      PlatformIO včasih samodejno zamenja pioarduino z uradnim
      framework-arduinoespressif32 @ 3.3.8. To povzroči:
      - napačen board profil (8MB Flash namesto 16MB, No PSRAM)
      - bootloop na Waveshare ESP32-S3-Touch-LCD-3.5B (OPI PSRAM bug)
      Rešitev: preveri kateri framework je nameščen in opozori.

HASH VREDNOST za esp-dsp:
  Vzeta iz managed_components/espressif__esp-dsp/dependencies.lock
  ob prvem uspešnem buildu (2026-05-16).
  Ob nadgradnji esp-dsp: posodobi ESP_DSP_COMPONENT_HASH spodaj.
"""

Import("env")
import os
import json
from pathlib import Path

# ============================================================
# KONSTANTE
# ============================================================

# Hash iz dependencies.lock ob prvem uspešnem buildu
ESP_DSP_COMPONENT_HASH = "12e44db246517a627bc0fe2b255b8335410c0215c98bfba2df5a8e3edea839ef"

# Relativna pot do managed_components (od project root)
ESP_DSP_HASH_FILE = Path("managed_components/espressif__esp-dsp/.component_hash")

# ============================================================
# FIX 1: esp-dsp .component_hash
# ============================================================
def fix_esp_dsp_symlink():
    hash_file = ESP_DSP_HASH_FILE

    if hash_file.exists():
        existing = hash_file.read_text(encoding="utf-8").strip()
        if existing == ESP_DSP_COMPONENT_HASH:
            print(f"[fix_build_env] esp-dsp .component_hash: OK ({ESP_DSP_COMPONENT_HASH[:16]}...)")
            return
        else:
            print(f"[fix_build_env] esp-dsp .component_hash: hash se razlikuje, posodabljam...")
    else:
        hash_file.parent.mkdir(parents=True, exist_ok=True)
        print(f"[fix_build_env] esp-dsp .component_hash: ustvarjam...")

    # Zapiši hash brez newline, UTF-8 brez BOM
    hash_file.write_bytes(ESP_DSP_COMPONENT_HASH.encode("utf-8"))
    print(f"[fix_build_env] esp-dsp .component_hash: OK (ustvarjen/posodobljen)")

# ============================================================
# FIX 2: .piopm BOM korupcija
# ============================================================
def fix_piopm_bom():
    pio_home = Path.home() / ".platformio"
    if "PLATFORMIO_HOME_DIR" in os.environ:
        pio_home = Path(os.environ["PLATFORMIO_HOME_DIR"])

    piopm_path = pio_home / "packages" / "framework-arduinoespressif32-libs" / ".piopm"

    if not piopm_path.exists():
        print(f"[fix_build_env] .piopm: datoteka ne obstaja ({piopm_path}) — preskoceno")
        return

    raw = piopm_path.read_bytes()

    BOM = b"\xef\xbb\xbf"
    if not raw.startswith(BOM):
        print(f"[fix_build_env] .piopm BOM: OK (ni BOM-a)")
        return

    print(f"[fix_build_env] .piopm BOM: najden, odstranjujem...")
    clean = raw[len(BOM):]

    try:
        json.loads(clean.decode("utf-8"))
    except json.JSONDecodeError as e:
        print(f"[fix_build_env] .piopm BOM: OPOZORILO — po odstranitvi JSON se vedno neveljaven: {e}")
        return

    piopm_path.write_bytes(clean)
    print(f"[fix_build_env] .piopm BOM: odstranjen, datoteka popravljena")

# ============================================================
# FIX 3: Preveri framework (opozorilo, ne avtomatska resitev)
# ============================================================
def check_framework():
    pio_home = Path.home() / ".platformio"
    if "PLATFORMIO_HOME_DIR" in os.environ:
        pio_home = Path(os.environ["PLATFORMIO_HOME_DIR"])

    pioarduino_platform = pio_home / "platforms" / "espressif32" / "platform.json"

    if not pioarduino_platform.exists():
        print("[fix_build_env] OPOZORILO: pioarduino platform.json ne najden!")
        print("[fix_build_env]   Pricakovano: ~/.platformio/platforms/espressif32/platform.json")
        print("[fix_build_env]   Mozni vzrok: PlatformIO je zamenjal pioarduino z uradnim espressif32")
        print("[fix_build_env]   RESITEV: pio run --target clean, nato pio run")
        return

    try:
        platform_data = json.loads(pioarduino_platform.read_text(encoding="utf-8"))
        version = platform_data.get("version", "neznano")
        name = platform_data.get("name", "neznano")
        if "55.03.38" in version or "pioarduino" in name.lower():
            print(f"[fix_build_env] framework: OK (pioarduino {version})")
        else:
            print(f"[fix_build_env] OPOZORILO: namesena platforma morda ni pioarduino!")
            print(f"[fix_build_env]   Najdeno: {name} @ {version}")
            print(f"[fix_build_env]   Pricakovano: pioarduino @ 55.03.38")
    except Exception as e:
        print(f"[fix_build_env] framework: ne morem prebrati platform.json: {e}")

# ============================================================
# FIX 4: framework-arduinoespressif32-libs package.json
# ============================================================
# Incident 2b: PlatformIO zahteva package.json v vsakem paketu.
# framework-arduinoespressif32-libs ga nima — MissingPackageManifestError.
# Podatki (name, version) so v .piopm ki vedno obstaja.
def fix_libs_package_json():
    pio_home = Path.home() / ".platformio"
    if "PLATFORMIO_HOME_DIR" in os.environ:
        pio_home = Path(os.environ["PLATFORMIO_HOME_DIR"])

    pkg_dir  = pio_home / "packages" / "framework-arduinoespressif32-libs"
    pj_path  = pkg_dir / "package.json"
    pm_path  = pkg_dir / ".piopm"

    if pj_path.exists():
        print("[fix_build_env] framework-arduinoespressif32-libs package.json: OK")
        return

    if not pm_path.exists():
        print("[fix_build_env] framework-arduinoespressif32-libs: paket ne obstaja, preskoceno")
        return

    # Preberi ime in verzijo iz .piopm
    try:
        pm_data = json.loads(pm_path.read_bytes().lstrip(b"\xef\xbb\xbf").decode("utf-8"))
        name    = pm_data.get("name", "framework-arduinoespressif32-libs")
        version = pm_data.get("version", "0.0.0")
    except Exception as e:
        name    = "framework-arduinoespressif32-libs"
        version = "5.5.4+sha.735507283d"
        print(f"[fix_build_env] .piopm branje napaka ({e}), uporabim privzete vrednosti")

    pkg = {
        "name": name,
        "version": version,
        "description": "Arduino ESP32 precompiled libraries (pioarduino 55.03.38)",
        "keywords": ["framework", "arduino", "espressif", "esp32"],
        "license": "LGPL-2.1-or-later",
        "repository": {"type": "git", "url": "https://github.com/espressif/arduino-esp32"}
    }
    pj_path.write_text(json.dumps(pkg, indent=2) + "\n", encoding="utf-8")
    print(f"[fix_build_env] framework-arduinoespressif32-libs package.json: ustvarjen ({version})")

# ============================================================
# FIX 5: esp32s3/ subdirektorij v framework-arduinoespressif32-libs
# ============================================================
# Incident 7 (2026-05-16): safe_framework_cleanup() pobrise esp32s3/ ce
# matching_custom_sdkconfig() vrne False (manjkajoc ali spremenjen sdkconfig).
# Brez esp32s3/ IDF compile vrze: "Missing Arduino sdkconfig template at esp32s3/sdkconfig"
# Resitev: ce esp32s3/ manjka a paket obstaja → opozori in zahtevaj brisanje paketa.
def check_esp32s3_subdir():
    pio_home = Path.home() / ".platformio"
    if "PLATFORMIO_HOME_DIR" in os.environ:
        pio_home = Path(os.environ["PLATFORMIO_HOME_DIR"])

    pkg_dir   = pio_home / "packages" / "framework-arduinoespressif32-libs"
    esp32s3   = pkg_dir / "esp32s3"
    sdkconfig = esp32s3 / "sdkconfig"

    if not pkg_dir.exists():
        print("[fix_build_env] esp32s3/: paket ne obstaja — PlatformIO bo prenesel ob buildu")
        return

    if not esp32s3.exists():
        print("[fix_build_env] KRITICNO: esp32s3/ subdirektorij manjka v framework-arduinoespressif32-libs!")
        print("[fix_build_env]   Vzrok: safe_framework_cleanup() je pobrisal esp32s3/ (sdkconfig mismatch)")
        print("[fix_build_env]   RESITEV: v PowerShell zbrisi cel paket:")
        print(f"[fix_build_env]     Remove-Item \"{pkg_dir}\" -Recurse -Force")
        print("[fix_build_env]   Nato: pio run  (PlatformIO bo prenesel paket in IDF prevedel ~14 min)")
        return

    if not sdkconfig.exists():
        print("[fix_build_env] OPOZORILO: esp32s3/sdkconfig manjka — IDF compile bo verjetno propadel!")
        print("[fix_build_env]   RESITEV: zbriši cel framework-arduinoespressif32-libs paket in pio run")
        return

    print("[fix_build_env] esp32s3/sdkconfig: OK")

# ============================================================
# MAIN — zazeni vse fixe
# ============================================================
print("[fix_build_env] === Preverjanje build okolja ===")
fix_esp_dsp_symlink()
fix_piopm_bom()
fix_libs_package_json()
check_esp32s3_subdir()
check_framework()
print("[fix_build_env] === Preverjanje koncano ===")
