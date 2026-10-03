"""Decode a Godot/CMake console log into a greppable UTF-8 twin.

Godot writes its console output as UTF-16LE, and the PowerShell capture script preserves that
encoding on purpose (it re-reads the raw child bytes). Reading such a file with a normal text tool
either fails outright or silently returns mojibake, which is how a real result gets mistaken for a
parse failure. Decoding to a .txt twin makes every downstream Read/Grep see plain text.

Usage: decode_log.py <file> [<file> ...]
"""

import sys
from pathlib import Path


def decode(path: Path) -> str:
    raw = path.read_bytes()
    for enc in ("utf-16-le", "utf-8", "cp936"):
        try:
            return raw.decode(enc)
        except (UnicodeDecodeError, UnicodeError):
            continue
    return raw.decode("utf-8", errors="replace")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    for name in argv[1:]:
        path = Path(name)
        if not path.exists():
            print(f"missing: {path}")
            continue
        text = decode(path).replace("\r", "")
        twin = path.with_suffix(path.suffix + ".txt")
        twin.write_text(text, encoding="utf-8")
        lines = text.strip().splitlines()
        print(f"=== {path} -> {twin} ({len(text)} chars, {len(lines)} lines) ===")
        print("\n".join(lines[-30:]))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
