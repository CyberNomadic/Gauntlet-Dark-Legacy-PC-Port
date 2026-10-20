#!/usr/bin/env python3
"""Run clang-tidy over every translation unit in compile_commands.json.

Findings in headers are reported once. Exit status is 1 when anything is
reported. Configure and build first so compile_commands.json exists. CI can
instead configure with CMAKE_CXX_SCAN_FOR_MODULES=OFF (no C++ modules are used)
and build only compile_commands, avoiding build-only module mapper files.

    python scripts/lint.py             # everything
    python scripts/lint.py src/game    # a subtree or single file
    python scripts/lint.py --shard-index 0 --shard-count 4  # first CI shard
    python scripts/lint.py --changed origin/main   # only what a change touches

--changed lints the translation units affected by the files that differ from a git
revision (or a range such as a..b): the changed sources themselves and every unit
that includes a changed header, directly or through other headers. The pre-push hook
under scripts/hooks runs it over what is being pushed; nothing affected passes.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

from lint_cache import CleanCache

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIAGNOSTIC = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*)$")


def find_clang_tidy() -> str:
    candidates = [
        os.environ.get("CLANG_TIDY"),
        shutil.which("clang-tidy"),
        r"C:\Program Files\LLVM\bin\clang-tidy.exe",
        "/usr/bin/clang-tidy",
        "/usr/local/bin/clang-tidy",
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).exists():
            return candidate
    sys.exit("clang-tidy not found; set CLANG_TIDY or put it on PATH")


def translation_units(filters: list[str]) -> list[pathlib.Path]:
    database = ROOT / "compile_commands.json"
    if not database.exists():
        sys.exit("compile_commands.json not found at the repository root; configure and build first")
    wanted = [(ROOT / f).resolve() for f in filters] or [ROOT / "src", ROOT / "tests"]
    units = []
    for entry in json.loads(database.read_text(encoding="utf-8")):
        path = pathlib.Path(entry["file"])
        if not path.is_absolute():
            path = pathlib.Path(entry["directory"]) / path
        path = path.resolve()
        if any(path == w or w in path.parents for w in wanted):
            units.append(path)
    return sorted(set(units))


INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl")


def changed_files(revision: str, git: str = "git") -> list[pathlib.Path]:
    """Sources that differ from `revision` (a commit, or a range such as a..b), including
    the working tree when a single revision is given."""
    completed = subprocess.run([git, "diff", "--name-only", "--diff-filter=ACMR", revision],
                               cwd=ROOT, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        sys.exit(f"git diff {revision} failed: {completed.stderr.strip()}")
    return [(ROOT / line.strip()).resolve() for line in completed.stdout.splitlines()
            if line.strip().endswith(SOURCE_SUFFIXES)]


def include_name(path: pathlib.Path) -> str:
    """How a header is written in an #include: its path under src or tests."""
    relative = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else path.as_posix()
    for prefix in ("src/", "tests/"):
        if relative.startswith(prefix):
            return relative[len(prefix):]
    return relative


def affected_units(roster: list[pathlib.Path], changed: list[pathlib.Path],
                   headers: list[pathlib.Path]) -> list[pathlib.Path]:
    """The roster units to lint for `changed`: changed units themselves, plus every unit
    that includes a changed header, following headers that include headers."""
    includes_of = {}
    for path in [*headers, *roster]:
        try:
            includes_of[path] = set(INCLUDE.findall(path.read_text(encoding="utf-8",
                                                                   errors="replace")))
        except OSError:
            includes_of[path] = set()
    header_set = set(headers)
    affected_names = {include_name(path) for path in changed if path in header_set}
    grown = True
    while grown:
        grown = False
        for header in headers:
            name = include_name(header)
            if name not in affected_names and includes_of[header] & affected_names:
                affected_names.add(name)
                grown = True
    changed_set = set(changed)
    return sorted(unit for unit in roster
                  if unit in changed_set or includes_of[unit] & affected_names)


def header_roster() -> list[pathlib.Path]:
    return sorted(path.resolve() for folder in ("src", "tests")
                  for path in (ROOT / folder).rglob("*") if path.suffix in (".h", ".hpp", ".inl"))


def select_shard(units: list[pathlib.Path], index: int, count: int) -> list[pathlib.Path]:
    """Partition the complete sorted roster, without depending on machine paths or hashes."""
    if count < 1 or not 0 <= index < count:
        raise ValueError("shard count must be positive and 0 <= shard index < shard count")
    return units[index::count]


def run_one(clang_tidy: str, unit: pathlib.Path) -> list[str]:
    command = [clang_tidy, "-p", str(ROOT), "--quiet",
               "--extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH",
               # The LLVM 18 analyzer crashes while inlining the MSVC 14.44 <format> sources;
               # keep it out of standard-library bodies.
               "--extra-arg=-Xclang", "--extra-arg=-analyzer-config",
               "--extra-arg=-Xclang", "--extra-arg=c++-stdlib-inlining=false",
               str(unit)]
    completed = subprocess.run(command, capture_output=True, text=True, cwd=ROOT, check=False)
    findings = []
    current = None
    for line in (completed.stdout + "\n" + completed.stderr).splitlines():
        match = DIAGNOSTIC.match(line)
        if match:
            current = line
            findings.append(line)
        elif current is not None and line.strip():
            findings[-1] += "\n" + line
    if completed.returncode != 0 and not findings:
        findings.append(f"{unit}: clang-tidy failed (exit {completed.returncode}):\n"
                        f"{completed.stdout.strip()}\n{completed.stderr.strip()}")
    return findings


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", help="source files or subtrees (default: src and tests)")
    parser.add_argument("--shard-index", type=int, default=0, help="zero-based CI shard index")
    parser.add_argument("--shard-count", type=int, default=1, help="number of disjoint CI shards")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                        help="maximum concurrent clang-tidy processes")
    parser.add_argument("--cache-dir", type=pathlib.Path,
                        help="reuse clean results after hashing clang dependencies (GCC-style commands)")
    parser.add_argument("--changed", metavar="REVISION",
                        help="lint only the units affected by the sources that differ from REVISION")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        select_shard([], args.shard_index, args.shard_count)
    except ValueError as error:
        parser.error(str(error))
    clang_tidy = find_clang_tidy()
    roster = translation_units(args.paths)
    if not roster:
        parser.error("no translation units found; check the paths and compilation database")
    if args.changed:
        roster = affected_units(roster, changed_files(args.changed), header_roster())
        if not roster:
            print(f"No translation units are affected by the changes since {args.changed}.")
            return 0
    units = select_shard(roster, args.shard_index, args.shard_count)
    started = time.monotonic()
    cache = None
    if args.cache_dir:
        try:
            cache = CleanCache(ROOT, args.cache_dir, clang_tidy)
        except (OSError, ValueError) as error:
            print(f"Lint cache unavailable; checking every unit: {error}", flush=True)

    def check(unit):
        key = cache.key(unit) if cache else None
        if key and cache.contains(key):
            return [], True
        findings = run_one(clang_tidy, unit)
        if not findings and key and cache.key(unit) == key:
            try:
                cache.remember(key)
            except OSError:
                pass  # an unwritable cache must not hide or fail a clean analysis
        return findings, False

    print(f"Lint shard {args.shard_index + 1}/{args.shard_count}: "
          f"{len(units)} of {len(roster)} translation units, {args.jobs} workers", flush=True)
    seen: set[str] = set()
    ordered: list[str] = []
    hits = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(check, unit): unit for unit in units}
        for done, future in enumerate(concurrent.futures.as_completed(pending), 1):
            unit = pending[future]
            try:
                findings, reused = future.result()
                hits += int(reused)
            except Exception as error:
                findings = [f"{unit}: clang-tidy could not complete: {error}"]
            label = unit.relative_to(ROOT) if unit.is_relative_to(ROOT) else unit
            print(f"[{done}/{len(units)}] {label} "
                  f"({time.monotonic() - started:.1f}s elapsed)", flush=True)
            for finding in findings:
                key = finding.split("\n", 1)[0]
                if key not in seen:
                    seen.add(key)
                    ordered.append(finding)
    for finding in sorted(ordered):
        print(finding)
        print()
    print(f"{len(units)} translation units checked ({hits} cached), {len(ordered)} findings "
          f"in {time.monotonic() - started:.1f}s", flush=True)
    return 1 if ordered else 0


if __name__ == "__main__":
    sys.exit(main())
