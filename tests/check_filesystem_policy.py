"""Keep Switch-incompatible recursive deletion behind the platform adapter."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
adapter = root / "source/platform/FileSystem.cpp"
violations = []
paths = [path for folder in ("source", "include") for path in (root / folder).rglob("*")
         if path.suffix in (".cpp", ".hpp", ".h")]
for path in sorted(paths):
    if path == adapter:
        continue
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if re.search(r"\bremove_all\s*\(", line.split("//", 1)[0]):
            violations.append(f"{path.relative_to(root)}:{number}: use platform::removePath and check its result")
if violations:
    print("\n".join(violations), file=sys.stderr)
    sys.exit(1)
print("Filesystem policy: passed")
