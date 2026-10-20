"""Validate design tokens and generate a deterministic C++20 header."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re


def generate(data: dict) -> str:
    themes = data["themes"]
    expected = {f"{aura}-{mode}" for aura in ("aurora", "ember", "monolith") for mode in ("dark", "light")}
    if {theme["id"] for theme in themes} != expected or len(themes) != 6:
        raise ValueError("Exactly six unique aura/theme combinations are required")
    def color(value: str) -> str:
        if not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
            raise ValueError(f"Invalid color: {value}")
        return "0xFF" + value[1:].upper() + "u"
    rows = []
    for theme in themes:
        values = [color(theme[key]) for key in ("background", "surface", "text", "muted", "accent")]
        if len(theme["mesh"]) != 3:
            raise ValueError("A mesh requires three colors")
        mesh = ", ".join(color(value) for value in theme["mesh"])
        rows.append('    Theme{"' + theme["id"] + '", ' + ", ".join(values) + ", {" + mesh + "}},")
    declarations = []
    for category in ("spacing", "radii", "motion"):
        declarations.append(f"namespace {category} {{")
        for name, value in data[category].items():
            if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
                raise ValueError(f"Invalid token identifier: {name}")
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 10000:
                raise ValueError(f"Invalid numeric token: {name}")
            declarations.append(f"inline constexpr float {name} = {float(value)}f;")
        declarations.append("}")
    return "\n".join([
        "// Generated from design/tokens.json. Do not edit.", "#pragma once", "#include <array>",
        "#include <cstdint>", "#include <string_view>", "namespace aura::design {",
        "struct Theme { std::string_view id; std::uint32_t background, surface, text, muted, accent;",
        "    std::array<std::uint32_t, 3> mesh; };", "inline constexpr std::array<Theme, 6> themes = {{",
        *rows, "}};", *declarations, "}", "",
    ])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = generate(json.loads(args.input.read_text(encoding="utf-8")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != result:
        args.output.write_text(result, encoding="utf-8", newline="\n")
