#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the iDrive app's text and image databases.

AAEntryNav_ALL_Texts.zip holds <lang>.txt ("id=text" lines, same format as BMW's PluginsCoded apps) and
AAEntryNav_ALL_Images.zip holds <imageId>.png, from hmiapp/res. Both are reproducible: same input, same bytes.

Usage: make_db.py <res dir> <output dir>
"""

import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

LANGS = {
    "de": "Deutsch",
    "en-UK": "English (UK)",
    "en-US": "English (US)",
    "es-ES": "Espanol",
    "es-US": "Espanol (US)",
    "fr-FR": "Francais",
    "fr-CA": "Francais (CA)",
    "it": "Italiano",
    "pt": "Portugues",
    "ru": "Russian",
}
TEXTS = {
    1: "Android Auto",
    2: "Android Auto: Connecting...",
    3: "Android Auto: Plug in phone",
    10: "Start Android Auto",
    11: "Stop Android Auto",
    13: "Connecting to the phone...",
    14: "Open Android Auto",
    15: "Start automatically",
    16: "Help",
    17: "Plug in the phone's USB cable",
    20: "NAV: Android Auto navigation",
    21: "MEDIA: Android Auto media",
    22: "COM / steering wheel phone button: Android Auto phone",
    23: "MENU: iDrive - OPTION: this menu - BACK: back",
    24: "Phone: wireless Android Auto on, paired with the car",
    25: "Flare Drive",
    26: "Choose a PIN for Flare Drive (4 to 8 digits)",
    27: "Enter the new PIN again",
    28: "Enter the Flare Drive PIN",
    29: "Wrong PIN. Try again",
    30: " ",  # blank title of the page held under Android Auto (no "Android Auto" tab during the volume)
    31: "The two PINs differed. Choose a PIN",
    32: "Scan this code with Flare Drive",
    33: "Too many wrong PINs. Try again later",
    34: "The car's Wi-Fi is off. Start Android Auto, then try again",
    35: "Not available while driving",
    36: "The PIN is 4 to 8 digits. Try again",
}
LANG_TEXTS = {  # ASCII only
    "fr-FR": {
        2: "Android Auto : connexion...",
        3: "Android Auto : brancher le telephone",
        10: "Demarrer Android Auto",
        11: "Arreter Android Auto",
        13: "Connexion au telephone...",
        14: "Ouvrir Android Auto",
        15: "Demarrage automatique",
        16: "Aide",
        17: "Branchez le cable USB du telephone",
        20: "NAV : navigation Android Auto",
        21: "MEDIA : musique Android Auto",
        22: "COM / bouton telephone du volant : telephone Android Auto",
        23: "MENU : iDrive - OPTION : ce menu - BACK : retour",
        24: "Telephone : Android Auto sans fil active, appaire a la voiture",
        26: "Choisissez un code PIN pour Flare Drive (4 a 8 chiffres)",
        27: "Saisissez a nouveau le nouveau code PIN",
        28: "Saisissez le code PIN de Flare Drive",
        29: "Code PIN incorrect. Reessayez",
        31: "Les deux codes PIN differaient. Choisissez un code PIN",
        32: "Scannez ce code avec Flare Drive",
        33: "Trop de codes PIN incorrects. Reessayez plus tard",
        34: "Le Wi-Fi de la voiture est coupe. Demarrez Android Auto, puis reessayez",
        35: "Indisponible pendant la conduite",
        36: "Le code PIN compte 4 a 8 chiffres. Reessayez",
    },
    "de": {
        2: "Android Auto: Verbinden...",
        3: "Android Auto: Telefon anschliessen",
        10: "Android Auto starten",
        11: "Android Auto beenden",
        13: "Verbindung zum Telefon...",
        14: "Android Auto oeffnen",
        15: "Automatisch starten",
        16: "Hilfe",
        17: "USB-Kabel des Telefons anschliessen",
        20: "NAV: Android Auto Navigation",
        21: "MEDIA: Android Auto Medien",
        22: "COM / Telefontaste am Lenkrad: Android Auto Telefon",
        23: "MENU: iDrive - OPTION: dieses Menue - BACK: zurueck",
        24: "Telefon: Android Auto kabellos an, mit dem Auto gekoppelt",
        26: "PIN fuer Flare Drive waehlen (4 bis 8 Ziffern)",
        27: "Neue PIN erneut eingeben",
        28: "PIN fuer Flare Drive eingeben",
        29: "Falsche PIN. Erneut versuchen",
        31: "Die beiden PINs waren verschieden. PIN waehlen",
        32: "Diesen Code mit Flare Drive scannen",
        33: "Zu viele falsche PINs. Spaeter erneut versuchen",
        34: "Das WLAN des Autos ist aus. Android Auto starten, dann erneut versuchen",
        35: "Waehrend der Fahrt nicht verfuegbar",
        36: "Die PIN hat 4 bis 8 Ziffern. Erneut versuchen",
    },
}
LANG_TEXTS["fr-CA"] = LANG_TEXTS["fr-FR"]
FIXED_DATE = (2026, 11, 2, 0, 0, 0)  # the same date in every archive: reproducible files


def add(archive, name, data):
    """Store one member, compressed, with the fixed date."""
    info = zipfile.ZipInfo(name, FIXED_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, data)


def check_xml_texts(xml):
    """Every textId the app's XML names has an English text, and every translation an English original."""
    named = {int(m.get("textId")) for m in ET.parse(xml).getroot().iter("textIdModel")}
    missing = sorted(named - set(TEXTS))
    if missing:
        raise SystemExit(f"FAIL: {xml.name} names texts with no English text: {missing}")
    for lang, texts in LANG_TEXTS.items():
        extra = sorted(set(texts) - set(TEXTS))
        if extra:
            raise SystemExit(f"FAIL: {lang} translates texts with no English text: {extra}")


def write_texts(path):
    """One <lang>.txt per language: English, overridden by the language's own texts."""
    check_xml_texts(Path(__file__).resolve().parent / "AAEntryNav.xml")
    with zipfile.ZipFile(path, "w") as archive:
        for lang, langname in LANGS.items():
            texts = dict(TEXTS)
            texts.update(LANG_TEXTS.get(lang, {}))
            body = f"100000={langname}\n" + "".join(f"{key}={value}\n" for key, value in sorted(texts.items()))
            if not body.isascii():
                raise SystemExit(f"FAIL: {lang} texts must stay ASCII")
            add(archive, lang + ".txt", body.encode("utf-8"))


def clear_png(w, h):
    # Fully transparent RGBA PNG (no image library): hides the big Android Auto logo of the blank state held under
    # Android Auto (seen while it is dimmed for the volume bar). An opaque black image looked like a window frame.
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    raw = b"".join(bytes(1 + 4 * w) for _ in range(h))  # filter byte 0 + transparent pixels, per row
    return (
        bytes([0x89])
        + b"PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def write_images(path, res):
    """The entry icon, the preview, the clear page background and the 8 frames of the connecting animation."""
    with zipfile.ZipFile(path, "w") as archive:
        add(archive, "55001.png", (res / "aa_row_icon_63x54.png").read_bytes())
        add(archive, "55002.png", (res / "aa_content_preview_549x484.png").read_bytes())
        add(archive, "55003.png", clear_png(549, 484))
        for frame in range(8):
            add(archive, f"{55010 + frame}.png", (res / "spinner" / f"spin_{frame}.png").read_bytes())


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: make_db.py <res dir> <output dir>")
    res, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    write_texts(out / "AAEntryNav_ALL_Texts.zip")
    write_images(out / "AAEntryNav_ALL_Images.zip", res)
    print("wrote AAEntryNav_ALL_Texts.zip, AAEntryNav_ALL_Images.zip in", out)


if __name__ == "__main__":
    main()
