"""The README's quick start is examples/quickstart.c, character for character (the build compiles that file, so
the code the README shows is code that builds).

  python test/quickstart_check.py        exit 1 with the first differing line when they differ

Compared: the lines between the README:begin and README:end marks of examples/quickstart.c, and the first C block
under "## Quick start" in README.md. Line ends are ignored (a Windows checkout may have CRLF).
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
lines = (root / "examples" / "quickstart.c").read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
begin = [i for i, ln in enumerate(lines) if "README:begin" in ln]
end = [i for i, ln in enumerate(lines) if "README:end" in ln]
if len(begin) != 1 or len(end) != 1 or end[0] <= begin[0]:
    sys.exit("examples/quickstart.c: one README:begin and one README:end mark are expected")
code = "\n".join(lines[begin[0] + 1:end[0]]) + "\n"

readme = (root / "README.md").read_text(encoding="utf-8").replace("\r\n", "\n")
section = re.search(r"(?ms)^## Quick start[ \t]*\n(.*?)(?=^## |\Z)", readme)
block = re.search(r"(?ms)^```c[ \t]*\n(.*?)^```[ \t]*$", section.group(1)) if section else None
if not block:
    sys.exit("README.md: no C block under \"## Quick start\"")
shown = block.group(1)
if shown != code:
    for n, (a, b) in enumerate(zip(shown.split("\n"), code.split("\n")), 1):
        if a != b:
            sys.exit(f"README.md's quick start differs from examples/quickstart.c at its line {n}:\n  README:  {a}\n  example: {b}")
    sys.exit("README.md's quick start and examples/quickstart.c differ in length")
print(f"README.md's quick start is examples/quickstart.c ({len(code.splitlines())} lines)")
