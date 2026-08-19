#!/usr/bin/env python3
"""
sfzflat.py - Aplanador (flattener) de archivos SFZ.

Toma cualquier SFZ (con #define, #include, default_path y la cascada
<global>/<master>/<group>/<region>) y produce una lista de <region>
"planos": cada uno con todos sus opcodes finales ya resueltos, sin
depender de ningun header padre.

El proceso tiene 3 fases, en este orden (importante, cada fase asume
que la anterior ya se aplico sobre el texto completo):

  1. preprocess()      -> resuelve #include y #define, texto SFZ puro.
  2. tokenize()         -> convierte el texto en una secuencia de eventos
                           (header, opcode=valor) sin importar si un
                           region se escribio en una linea o en varias.
  3. flatten_events()   -> recorre los eventos aplicando la herencia
                           global > master > group > region y la
                           normalizacion de sample= via default_path.

Opcionalmente, clean_opcodes() filtra el resultado para dejar solo una
lista basica de opcodes (ver ALLOWED_OPCODES).

Referencias de comportamiento real:
  https://github.com/sfztools/sfizz/blob/develop/devtools/Preprocessor.cpp
  https://github.com/sfzlab/sfz-tools-cli
  https://github.com/sfztools/sfz-flat/
"""

from __future__ import annotations

import argparse
import os
import re
import sys


# ---------------------------------------------------------------------------
# Fase 1: preprocesado (#define, #include, comentarios)
# ---------------------------------------------------------------------------

DEFINE_RE = re.compile(r'^#define\s+(\$\w+)\s+(.+?)\s*$')
INCLUDE_RE = re.compile(r'^#include\s+"([^"]+)"\s*$')


def read_sfz_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    # SFZ siempre usa "/" como separador de rutas, sin importar el SO
    # en que se escribio el archivo -- normalizamos de una vez.
    return text.replace("\\", "/")


def strip_comment(line: str) -> str:
    """Elimina un comentario // hasta el final de la linea, respetando
    comillas (para no cortar un #include "ruta//rara" por accidente)."""
    in_quotes = False
    for i in range(len(line) - 1):
        c = line[i]
        if c == '"':
            in_quotes = not in_quotes
        elif not in_quotes and c == "/" and line[i + 1] == "/":
            return line[:i]
    return line


def apply_defines(text: str, defines: dict) -> str:
    """Sustituye cada $NOMBRE por su valor. El lookahead (?!\\w) evita que
    $EXT reemplace por accidente dentro de $EXTRA."""
    for name, value in defines.items():
        text = re.sub(re.escape(name) + r"(?!\w)", value, text)
    return text


def preprocess(text: str, base_dir: str, defines: dict, _include_stack=None) -> str:
    """Aplica #define y #include de forma recursiva y devuelve el SFZ ya
    "puro" (sin directivas, sin comentarios).

    `defines` se pasa por referencia y se comparte durante todo el arbol
    de includes: un #define hecho antes de un #include es visible dentro
    del archivo incluido, y uno hecho dentro del include sigue vigente al
    volver al archivo padre -- igual que un preprocesador de una sola
    pasada (estilo C).
    """
    if _include_stack is None:
        _include_stack = set()

    out_lines = []
    for raw_line in text.splitlines():
        line = strip_comment(raw_line)
        line = apply_defines(line, defines)
        stripped = line.strip()

        m = DEFINE_RE.match(stripped)
        if m:
            name, value = m.group(1), m.group(2).strip()
            defines[name] = value
            continue

        m = INCLUDE_RE.match(stripped)
        if m:
            inc_path = m.group(1)
            full_path = os.path.normpath(os.path.join(base_dir, inc_path))
            if full_path in _include_stack:
                raise ValueError(f"Include circular detectado: {full_path}")
            if not os.path.isfile(full_path):
                raise FileNotFoundError(f"No se encontro el include: {full_path}")

            inc_text = read_sfz_text(full_path)
            _include_stack.add(full_path)
            inc_processed = preprocess(
                inc_text, os.path.dirname(full_path), defines, _include_stack
            )
            _include_stack.discard(full_path)
            out_lines.append(inc_processed)
            continue

        out_lines.append(line)

    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
# Fase 2: tokenizado (headers + opcode=valor, lectura vertical u horizontal)
# ---------------------------------------------------------------------------

# Un opcode es una clave seguida de "=". El valor es todo lo que sigue
# hasta el proximo token (otro opcode, un header, o el final del texto),
# sin importar si hay saltos de linea de por medio. Esto permite valores
# con espacios (rutas de sample, labels de CC, etc.) sin necesitar comillas.
_TOKEN_RE = re.compile(r"<(?P<header>\w+)>|(?P<key>[A-Za-z_]\w*)=")


def tokenize(text: str):
    """Devuelve una lista de eventos en orden de aparicion:
    ('header', nombre) o ('opcode', clave, valor)."""
    matches = list(_TOKEN_RE.finditer(text))
    events = []
    for i, m in enumerate(matches):
        next_start = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        if m.group("header") is not None:
            events.append(("header", m.group("header").lower()))
        else:
            value = text[m.end():next_start].strip()
            events.append(("opcode", m.group("key").lower(), value))
    return events


# ---------------------------------------------------------------------------
# Fase 3: aplanado (herencia global > master > group > region + default_path)
# ---------------------------------------------------------------------------

CASCADE_LEVELS = ("global", "master", "group")


def flatten_events(events) -> list[dict]:
    """Aplica la cascada de opcodes y la normalizacion de sample= segun el
    default_path activo, devolviendo una lista de dicts (uno por region),
    en el orden en que aparecen las regiones en el archivo.

    Reglas de la cascada (igual que el motor SFZ real):
      - <global>  resetea master y group, y se convierte en la nueva base.
      - <master>  resetea group.
      - <group>   no hereda del <group> anterior, solo de master/global.
      - <region>  parte vacio y se combina con global+master+group; sus
                  propios opcodes tienen la ultima palabra.
      - <control> no participa de la cascada; solo su default_path afecta
                  (reemplazandolo, no concatenandolo) a las regiones que
                  se definan despues.
    Otros headers (<curve>, <effect>, <midi>, ...) se preservan tal cual,
    como bloques aparte, ya que no forman parte de la herencia de regiones.
    """
    cascade = {"global": {}, "master": {}, "group": {}}
    current_header = None
    current_default_path = ""
    region_opcodes = None
    regions: list[dict] = []
    other_blocks: list[tuple[str, dict]] = []

    def close_region():
        nonlocal region_opcodes
        if region_opcodes is None:
            return
        merged: dict = {}
        for level in CASCADE_LEVELS:
            merged.update(cascade[level])
        merged.update(region_opcodes)
        if current_default_path and "sample" in merged:
            merged["sample"] = current_default_path + merged["sample"]
        if "sample" in merged:
            # sample= primero, por legibilidad -- el resto conserva su orden
            merged = {"sample": merged.pop("sample"), **merged}
        regions.append(merged)
        region_opcodes = None

    for ev in events:
        if ev[0] == "header":
            name = ev[1]
            close_region()
            current_header = name
            if name == "global":
                cascade["global"] = {}
                cascade["master"] = {}
                cascade["group"] = {}
            elif name == "master":
                cascade["master"] = {}
                cascade["group"] = {}
            elif name == "group":
                cascade["group"] = {}
            elif name == "region":
                region_opcodes = {}
            elif name not in ("control",):
                other_blocks.append((name, {}))
            continue

        _, key, value = ev
        if current_header == "control":
            if key == "default_path":
                current_default_path = value
        elif current_header == "region":
            region_opcodes[key] = value
        elif current_header in cascade:
            cascade[current_header][key] = value
        elif other_blocks and other_blocks[-1][0] == current_header:
            other_blocks[-1][1][key] = value

    close_region()
    return regions, other_blocks


# ---------------------------------------------------------------------------
# Limpiador opcional de opcodes
# ---------------------------------------------------------------------------

ALLOWED_OPCODES = {
    "sample", "pitch_keycenter", "lokey", "hikey", "lovel", "hivel", "key",
    "tune", "offset", "end", "pan", "volume",
    "seq_length", "seq_position", "lorand", "hirand",
    "loop_mode", "loop_type", "direction", "loop_start", "loop_end", "loop_tune",
    "xfin_lokey", "xfin_hikey", "xfout_lokey", "xfout_hikey",
    "xfin_lovel", "xfin_hivel", "xfout_lovel", "xfout_hivel",
    "xf_keycurve", "xf_velcurve",
}


def clean_opcodes(regions: list[dict], allowed=ALLOWED_OPCODES) -> list[dict]:
    return [{k: v for k, v in region.items() if k in allowed} for region in regions]


# ---------------------------------------------------------------------------
# Render de vuelta a texto SFZ
# ---------------------------------------------------------------------------

def render_sfz(regions: list[dict], other_blocks: list[tuple[str, dict]] | None = None) -> str:
    lines = []
    for region in regions:
        parts = " ".join(f"{k}={v}" for k, v in region.items())
        lines.append(f"<region> {parts}".rstrip())
    for name, opcodes in other_blocks or []:
        parts = " ".join(f"{k}={v}" for k, v in opcodes.items())
        lines.append(f"<{name}> {parts}".rstrip())
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# API de alto nivel
# ---------------------------------------------------------------------------

def flatten_sfz(path: str, clean: bool = False) -> str:
    base_dir = os.path.dirname(os.path.abspath(path))
    raw = read_sfz_text(path)

    defines: dict = {}
    preprocessed = preprocess(raw, base_dir, defines)

    events = tokenize(preprocessed)
    regions, other_blocks = flatten_events(events)

    if clean:
        regions = clean_opcodes(regions)

    return render_sfz(regions, other_blocks)


def main():
    parser = argparse.ArgumentParser(description="Aplana un archivo SFZ en <region> individuales.")
    parser.add_argument("sfz_file", help="Archivo .sfz de entrada")
    parser.add_argument("-o", "--output", help="Archivo de salida (por defecto, stdout)")
    parser.add_argument(
        "--clean", action="store_true",
        help="Filtra el resultado dejando solo la lista basica de opcodes (ver ALLOWED_OPCODES)",
    )
    args = parser.parse_args()

    result = flatten_sfz(args.sfz_file, clean=args.clean)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(result)
    else:
        sys.stdout.write(result)


if __name__ == "__main__":
    main()
