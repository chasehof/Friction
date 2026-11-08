#!/usr/bin/env python3
"""Build the single-header distribution from the modular headers.

Users who want one file to drop in a project get `single_include/friction/
friction.hpp`. It is generated, not written by hand, so the two never drift: the
tests compile against both spellings.

The transformation is deliberately boring. Local includes are replaced by the
text of the header they name, in dependency order; system includes are hoisted to
the top and de-duplicated; everything else is emitted verbatim. Because the
library is header-only and every function is `inline`, the result needs no other
work to be correct.

Usage:
    tools/amalgamate.py [--check]

`--check` exits non-zero when the generated header is out of date, which is what
CI wants.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPOSITORY / "include"
ENTRY_HEADER = "friction/friction.hpp"
OUTPUT_PATH = REPOSITORY / "single_include" / "friction" / "friction.hpp"

# Headers of this library are included by path, quoted or angled depending on
# who is including them; both spellings mean "splice me in".
ANY_LOCAL_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:"([^"]+)"|<((?:friction/)[^>]+)>)\s*$', re.MULTILINE
)
SYSTEM_INCLUDE = re.compile(r"^\s*#\s*include\s*<([^>]+)>\s*$", re.MULTILINE)
PRAGMA_ONCE = re.compile(r"^\s*#\s*pragma\s+once\s*$", re.MULTILINE)
BANNER = """\
//
//  Friction {version} -- single-header distribution.
//
//  Generated from include/friction/ by tools/amalgamate.py. Do not edit.
//  See the repository for the sources and the documentation.
//
"""


class AmalgamationError(RuntimeError):
    """Raised when the header set does not describe a valid amalgamation."""


def read_version() -> str:
    """Reads the version out of the umbrella header so the banner cannot lie."""
    umbrella = (SOURCE_ROOT / ENTRY_HEADER).read_text(encoding="utf-8")
    parts = re.findall(
        r"#define FRICTION_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)", umbrella
    )
    if len(parts) != 3:
        raise AmalgamationError(f"could not read the version from {ENTRY_HEADER}")
    numbers = dict(parts)
    return "{}.{}.{}".format(
        numbers["MAJOR"], numbers["MINOR"], numbers["PATCH"]
    )


def collect(entry: str) -> list[str]:
    """Returns every header reachable from `entry`, dependencies first."""
    ordered: list[str] = []
    visiting: set[str] = set()

    def visit(relative: str) -> None:
        if relative in ordered:
            return
        if relative in visiting:
            raise AmalgamationError(f"include cycle through {relative}")
        visiting.add(relative)

        path = SOURCE_ROOT / relative
        if not path.is_file():
            raise AmalgamationError(f"no such header: {relative}")

        for match in ANY_LOCAL_INCLUDE.finditer(path.read_text(encoding="utf-8")):
            visit(match.group(1) or match.group(2))

        visiting.discard(relative)
        ordered.append(relative)

    visit(entry)
    return ordered


def strip_prologue(text: str) -> str:
    """Removes the parts that only make sense in a file of its own."""
    lines = [
        line
        for line in text.splitlines()
        if not PRAGMA_ONCE.match(line) and not ANY_LOCAL_INCLUDE.match(line)
    ]
    while lines and not lines[0].strip():
        lines.pop(0)
    return "\n".join(lines).rstrip() + "\n"


def system_includes(headers: list[str]) -> list[str]:
    """Standard and platform headers the amalgamation depends on, sorted.

    Only includes that sat outside every conditional block are hoisted. Anything
    inside one -- `<fcntl.h>` behind an `#if` for Linux, say -- stays where it
    was, because lifting it to the top would ask a Windows compiler for a header
    that does not exist there.
    """
    found: set[str] = set()
    for relative in headers:
        depth = 0
        for line in (SOURCE_ROOT / relative).read_text(encoding="utf-8").splitlines():
            if re.match(r"^\s*#\s*(if|ifdef|ifndef)\b", line):
                depth += 1
                continue
            if re.match(r"^\s*#\s*endif\b", line):
                depth = max(0, depth - 1)
                continue
            if depth != 0:
                continue
            found.update(
                name
                for name in SYSTEM_INCLUDE.findall(line)
                if not name.startswith("friction/")
            )
    return sorted(found)


def build() -> str:
    version = read_version()
    headers = collect(ENTRY_HEADER)

    parts = [BANNER.format(version=version), "#pragma once", ""]
    includes = system_includes(headers)
    if includes:
        parts.append("\n".join(f"#include <{name}>" for name in includes))
        parts.append("")

    for relative in headers:
        body = strip_prologue((SOURCE_ROOT / relative).read_text(encoding="utf-8"))
        parts.append(f"// ============ {relative} ============\n")
        parts.append(body)
        parts.append("")

    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if the generated header is out of date",
    )
    arguments = parser.parse_args()

    try:
        generated = build()
    except AmalgamationError as error:
        print(f"amalgamate: {error}", file=sys.stderr)
        return 2

    if arguments.check:
        if not OUTPUT_PATH.is_file():
            print(f"amalgamate: {OUTPUT_PATH.relative_to(REPOSITORY)} does not exist", file=sys.stderr)
            return 1
        if OUTPUT_PATH.read_text(encoding="utf-8") != generated:
            print(
                f"amalgamate: {OUTPUT_PATH.relative_to(REPOSITORY)} is out of date;"
                " run tools/amalgamate.py",
                file=sys.stderr,
            )
            return 1
        print(f"amalgamate: {OUTPUT_PATH.relative_to(REPOSITORY)} is up to date")
        return 0

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(generated, encoding="utf-8")
    print(
        f"amalgamate: wrote {OUTPUT_PATH.relative_to(REPOSITORY)}"
        f" ({len(generated.splitlines())} lines)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
