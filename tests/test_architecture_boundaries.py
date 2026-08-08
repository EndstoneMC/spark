#!/usr/bin/env python3
"""Verify that source layers respect architectural boundaries.

Dependency model: platform/endstone -> application -> core -> native

- src/native/      may only include from src/native/
- src/core/        may include from src/core/ and src/native/
- src/application/ may include from src/application/, src/core/, src/native/
- src/platform/    may include from anywhere
- src/plugin.cpp   may include from anywhere

No layer below platform/ may include <endstone/...> or "platform/endstone/...".
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

# Layers and what they may include (prefixes allowed).
LAYER_RULES = {
    "native": {"native/"},
    "core": {"core/", "native/"},
    "application": {"application/", "core/", "native/"},
}

FORBIDDEN_PATTERNS = [
    re.compile(r"^endstone/"),
    re.compile(r"^platform/endstone/"),
    re.compile(r"^platform/"),
]


def layer_of(path: Path) -> str | None:
    rel = path.relative_to(SRC)
    parts = rel.parts
    if parts[0] == "native":
        return "native"
    if parts[0] == "core":
        return "core"
    if parts[0] == "application":
        return "application"
    return None


def check_file(path: Path) -> list[str]:
    layer = layer_of(path)
    if layer is None:
        return []
    allowed = LAYER_RULES[layer]
    violations = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in INCLUDE_RE.finditer(text):
        inc = m.group(1)
        # System/library includes (no slash or known external libs) are always fine.
        if "/" not in inc and not inc.startswith("endstone"):
            continue
        # Check forbidden patterns first.
        for pat in FORBIDDEN_PATTERNS:
            if pat.match(inc):
                violations.append(f"{path.relative_to(ROOT)}: includes <{inc}> (forbidden in {layer} layer)")
                break
        else:
            # Check if the include is allowed by the layer's rules.
            # spark-internal includes use paths like "core/...", "native/...", etc.
            if not any(inc.startswith(prefix) for prefix in allowed):
                # External library includes (cpptrace, etc.) don't match any
                # internal prefix and are fine.
                internal_prefixes = ("core/", "native/", "application/", "platform/")
                if not any(inc.startswith(p) for p in internal_prefixes):
                    continue
                violations.append(
                    f"{path.relative_to(ROOT)}: includes <{inc}> (not allowed in {layer} layer)"
                )
    return violations


def main() -> int:
    extensions = {".h", ".hpp", ".cpp", ".cc"}
    files = [p for p in SRC.rglob("*") if p.suffix in extensions and layer_of(p) is not None]
    violations = []
    for f in sorted(files):
        violations.extend(check_file(f))
    if violations:
        for v in violations:
            print(v, file=sys.stderr)
        print(f"\n{len(violations)} architectural boundary violation(s) found.", file=sys.stderr)
        return 1
    print(f"OK: {len(files)} files checked, no boundary violations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
