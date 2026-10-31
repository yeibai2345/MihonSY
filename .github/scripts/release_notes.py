"""Extract an exact version section, without ever publishing empty language headings."""
import re
import sys
from pathlib import Path


def section(path: Path, version: str) -> str:
    if not path.exists():
        return ""
    match = re.search(
        rf"^## v{re.escape(version)}(?:\s[^\n]*)?\n(.*?)(?=^## |\Z)",
        path.read_text(encoding="utf-8"),
        re.MULTILINE | re.DOTALL,
    )
    return match.group(1).strip() if match else ""


def notes(version: str, root: Path = Path(".")) -> str:
    languages = [("中文 / Chinese", "CHANGELOG.md"), ("English", "CHANGELOG.en.md")]
    blocks = [f"## {label}\n\n{text}" for label, filename in languages if (text := section(root / filename, version))]
    return "\n\n---\n\n".join(blocks) or f"MihonSY {version}\n\nBuilt from the tagged source commit."


if __name__ == "__main__":
    print(notes(sys.argv[1]))
