#!/usr/bin/env python3
"""Stage a portable game folder with an unchanged original retail asset tree.

    python scripts/release/package.py --output out/gauntlet --dry-run
    python scripts/release/package.py --output out/gauntlet --assets /disc/Gauntlet

Uses the existing Release build; never builds, exports, deletes or replaces files.
The output must not exist. Gauntlet and its sibling carddemo are copied in full,
alongside the executable, compiled shaders, shipped data and runtime libraries.
Windows release game builds link the CRT statically; ordinary and delay-load PE
imports are checked before packaging. Dynamic-CRT development presets retain
their redistributable runtime. Extra dependencies may be supplied with --runtime-dir.
The installer has its own independent runtime dependencies.
The Vulkan driver remains a system prerequisite.
"""

import argparse
from dataclasses import dataclass
import os
import pathlib
import re
import shutil
import stat
import struct
import sys
from typing import Optional

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))  # the top-level helpers
import build
import devenv


@dataclass(frozen=True)
class PackagePlan:
    output: pathlib.Path
    files: tuple[tuple[pathlib.Path, pathlib.Path], ...]
    directories: tuple[pathlib.Path, ...]


def child_directory(parent: pathlib.Path, name: str) -> pathlib.Path:
    """Find an original folder without changing its case-sensitive spelling."""
    matches = [path for path in parent.iterdir()
               if path.name.casefold() == name.casefold() and path.is_dir()]
    if len(matches) != 1:
        raise ValueError(f"Expected one {name} directory under {parent}, found {len(matches)}")
    return matches[0]


def require_file(path: pathlib.Path) -> None:
    if not path.is_file():
        raise ValueError(f"Required package input is missing: {path}")


def validate_output(output: pathlib.Path, sources: list[pathlib.Path]) -> pathlib.Path:
    """Refuse existing outputs and any location inside a source tree, including aliases."""
    if os.path.lexists(output):
        raise ValueError(f"Output already exists; choose a new directory: {output}")
    resolved = output.resolve()
    for source in sources:
        source = source.resolve()
        if resolved == source or source in resolved.parents:
            raise ValueError(f"Output must not be inside a package input: {source}")
    return resolved


def runtime_library(path: pathlib.Path) -> bool:
    name = path.name.casefold()
    return name.endswith((".dll", ".dylib", ".so")) or ".so." in name


def crt_library(name: str) -> bool:
    """Dynamic Microsoft CRT families, including API-set and debug variants."""
    return bool(re.fullmatch(
        r"(?:(?:vcruntime|msvcp|msvcr|concrt|vccorlib)\d[^/\\]*|msvcrt|ucrtbased?|api-ms-win-crt-[^/\\]+)\.dll",
        name, re.IGNORECASE))


def pe_imports(path: pathlib.Path) -> tuple[str, ...]:
    """Read bounded PE32+ import names, including delay-load imports, without loading code.

    This checks the game's direct imports, not the transitive dependencies of
    system graphics/audio DLLs or the separate frozen installer executable.
    """
    size = path.stat().st_size
    with path.open("rb") as stream:
        def read(offset, count):
            if offset < 0 or count < 0 or offset > size or count > size - offset:
                raise ValueError(f"Truncated PE import data: {path}")
            stream.seek(offset)
            value = stream.read(count)
            if len(value) != count:
                raise ValueError(f"Truncated PE import data: {path}")
            return value

        dos = read(0, 64)
        if dos[:2] != b"MZ":
            raise ValueError(f"Expected an x64 Windows PE executable: {path}")
        pe = struct.unpack_from("<I", dos, 60)[0]
        header = read(pe, 24)
        machine, sections = struct.unpack_from("<HH", header, 4)
        optional_size = struct.unpack_from("<H", header, 20)[0]
        if header[:4] != b"PE\0\0" or machine != 0x8664 or not 1 <= sections <= 96 or optional_size < 128:
            raise ValueError(f"Unsupported or invalid x64 PE header: {path}")
        optional = read(pe + 24, optional_size)
        directory_count = struct.unpack_from("<I", optional, 108)[0]
        if (struct.unpack_from("<H", optional)[0] != 0x20b or not 2 <= directory_count <= 16 or
                112 + directory_count * 8 > optional_size):
            raise ValueError(f"Invalid PE32+ data directories: {path}")
        image_base = struct.unpack_from("<Q", optional, 24)[0]
        headers_size = struct.unpack_from("<I", optional, 60)[0]
        section_table = read(pe + 24 + optional_size, sections * 40)
        ranges = []
        for offset in range(0, len(section_table), 40):
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", section_table, offset + 8)
            if raw > size or raw_size > size - raw:
                raise ValueError(f"Invalid PE section bounds: {path}")
            ranges.append((address, max(virtual_size, raw_size), raw, raw_size))

        def rva(value, count):
            matches = [(raw + value - address, raw_size - (value - address))
                       for address, length, raw, raw_size in ranges
                       if address <= value < address + length and value - address < raw_size]
            if value < headers_size:
                matches.append((value, headers_size - value))
            if len(matches) != 1 or count > matches[0][1]:
                raise ValueError(f"Unmapped or ambiguous PE import address: {path}")
            return read(matches[0][0], count)

        def name(address):
            result = bytearray()
            for offset in range(260):
                value = rva(address + offset, 1)[0]
                if value == 0:
                    try:
                        text = result.decode("ascii")
                    except UnicodeError as error:
                        raise ValueError(f"Non-ASCII PE import name: {path}") from error
                    if not re.fullmatch(r"[A-Za-z0-9_.-]+\.dll", text, re.IGNORECASE):
                        raise ValueError(f"Invalid PE import library name: {path}")
                    return text.casefold()
                result.append(value)
            raise ValueError(f"Unterminated PE import library name: {path}")

        imports = set()
        for directory, stride in ((1, 20), (13, 32)):
            if directory >= directory_count:
                continue
            address, length = struct.unpack_from("<II", optional, 112 + directory * 8)
            if address == 0 and length == 0:
                continue
            if address == 0 or not stride <= length <= 1024 * 1024:
                raise ValueError(f"Invalid PE import directory size: {path}")
            terminated = False
            for offset in range(0, length - stride + 1, stride):
                entry = struct.unpack(f"<{stride // 4}I", rva(address + offset, stride))
                if not any(entry):
                    terminated = True
                    break
                if directory == 13:
                    if entry[0] not in (0, 1):
                        raise ValueError(f"Invalid PE delay-import attributes: {path}")
                    library = entry[1] if entry[0] == 1 else entry[1] - image_base
                else:
                    library = entry[3]
                imports.add(name(library))
            if not terminated:
                raise ValueError(f"Unterminated PE import directory: {path}")
        return tuple(sorted(imports))


def require_static_crt(executable: pathlib.Path) -> None:
    imports = pe_imports(executable)
    dynamic = [name for name in imports if crt_library(name)]
    if dynamic:
        raise ValueError(f"Windows game must use the static CRT; dynamic imports: {', '.join(dynamic)}")


def windows_static_crt(binary: pathlib.Path) -> bool:
    """Select the verified build contract, never infer linkage from nearby DLLs."""
    if not (binary / "CMakeCache.txt").is_file():
        raise ValueError(f"Configure the selected Windows preset before packaging: {binary}")
    triplet = str(build.cache_path(binary, "VCPKG_TARGET_TRIPLET", pathlib.Path()))
    runtime = str(build.cache_path(binary, "CMAKE_MSVC_RUNTIME_LIBRARY", pathlib.Path()))
    if triplet == "x64-windows-static" and runtime == "MultiThreaded":
        return True
    if triplet == "x64-windows" and runtime in (
            ".", "MultiThreadedDLL", "MultiThreadedDebugDLL", "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"):
        return False
    raise ValueError(f"Unsupported or mixed Windows runtime contract: {triplet}, {runtime}; reconfigure --fresh")


def linked_entry(path: pathlib.Path) -> bool:
    """Reject links and Windows reparse points, including junctions on Python 3.9-3.11."""
    info = path.lstat()
    return (stat.S_ISLNK(info.st_mode) or
            bool(getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT))


def make_plan(executable: pathlib.Path, assets: Optional[pathlib.Path], data: pathlib.Path,
              output: pathlib.Path, runtime_dirs: tuple[pathlib.Path, ...] = (), *,
              static_crt: bool = False) -> PackagePlan:
    """Validate all inputs and enumerate the exact copy, without writing anything."""
    executable = executable.resolve()
    assets = assets.resolve() if assets else None
    data = data.resolve()
    require_file(executable)
    if static_crt:
        require_static_crt(executable)
    if assets and (assets.name.casefold() != "gauntlet" or not assets.is_dir()):
        raise ValueError(f"--assets must name the original Gauntlet directory: {assets}")
    carddemo = child_directory(assets.parent, "carddemo") if assets else None
    shaders = executable.parent / "shaders"
    for shader in ("immediate.vert.spv", "immediate.frag.spv", "postprocess.vert.spv",
                   "depth_of_field.frag.spv", "depth_of_field_ms.frag.spv",
                   "bloom.frag.spv", "bloom_ms.frag.spv",
                   "ambient_occlusion.frag.spv", "ambient_occlusion_ms.frag.spv"):
        require_file(shaders / shader)
    require_file(data / "config.json")
    require_file(data / "text/en.json")
    for directory in runtime_dirs:
        if not directory.is_dir():
            raise ValueError(f"Runtime directory not found: {directory}")
    inputs = [executable.parent, data, *runtime_dirs]
    if assets:
        inputs.append(assets.parent)
    output = validate_output(output, inputs)

    files = []
    directories = set()
    destinations = {}

    def add_file(source, destination):
        require_file(source)
        key = destination.as_posix().casefold()
        if key in destinations:
            if destinations[key].resolve() == source.resolve():
                return
            raise ValueError(f"Conflicting package destination: {destination}")
        destinations[key] = source
        files.append((source, destination))

    def add_tree(source, destination):
        def fail_walk(error):
            raise error

        if linked_entry(source):
            raise ValueError(f"Linked entry inside package input: {source}")
        directories.add(destination)
        for folder, subdirs, names in os.walk(source, followlinks=False, onerror=fail_walk):
            folder = pathlib.Path(folder)
            relative = folder.relative_to(source)
            for name in subdirs + names:
                path = folder / name
                # Junctions and symlinks are not original disc records. Refuse to
                # follow them into unrelated files or silently omit their contents.
                if linked_entry(path):
                    raise ValueError(f"Linked entry inside package input: {path}")
            for name in subdirs:
                directories.add(destination / relative / name)
            for name in sorted(names):
                add_file(folder / name, destination / relative / name)

    add_file(executable, pathlib.Path(executable.name))
    for directory in (executable.parent, *runtime_dirs):
        for path in sorted(directory.iterdir()):
            if path.is_file() and runtime_library(path):
                if static_crt and crt_library(path.name):
                    continue # A stale developer CRT is not part of the statically linked game.
                add_file(path, pathlib.Path(path.name))
    add_tree(shaders, pathlib.Path("shaders"))
    add_tree(data, pathlib.Path("data"))
    if assets:
        add_tree(assets, pathlib.Path(assets.name))
        add_tree(carddemo, pathlib.Path(carddemo.name))
    return PackagePlan(output, tuple(files), tuple(sorted(directories)))


def stage(plan: PackagePlan) -> None:
    """Create the new package, using exclusive writes so no existing file is overwritten."""
    plan.output.mkdir(parents=True, exist_ok=False)
    for directory in plan.directories:
        (plan.output / directory).mkdir(parents=True, exist_ok=True)
    for source, relative in plan.files:
        destination = plan.output / relative
        with source.open("rb") as reader, destination.open("xb") as writer:
            shutil.copyfileobj(reader, writer)
        shutil.copystat(source, destination)


def msvc_runtime_directory() -> pathlib.Path:
    """Use the installed redistributable CRT, never DLLs harvested from System32."""
    environment = devenv.environment()
    redist = environment.get("VCToolsRedistDir")
    candidates = sorted((pathlib.Path(redist) / "x64").glob("Microsoft.VC*.CRT")) if redist else []
    if not candidates:
        raise ValueError("MSVC redistributable CRT not found; supply its x64 directory "
                         "with --runtime-dir")
    return candidates[-1]


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", default=devenv.release_preset())
    parser.add_argument("--executable", type=pathlib.Path, help="override the preset's executable")
    parser.add_argument("--assets", type=pathlib.Path, help="original Gauntlet tree; carddemo is its sibling")
    parser.add_argument("--data", type=pathlib.Path, help="shipped configuration/text defaults")
    parser.add_argument("--runtime-dir", type=pathlib.Path, action="append", default=[],
                        help="extra runtime-library directory; static builds exclude CRT DLLs (repeatable)")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="new, nonexistent package directory")
    parser.add_argument("--dry-run", action="store_true", help="validate and list the copy without writing")
    args = parser.parse_args(argv)
    try:
        presets = build.build_presets()
        if args.preset not in presets:
            raise ValueError(f"Unknown build preset: {args.preset}")
        binary = devenv.ROOT / "build" / presets[args.preset]
        executable = args.executable or binary / "bin" / f"gauntlet{build.EXE}"
        assets = args.assets or build.cache_path(binary, "GDL_ASSET_DIR",
                                                 devenv.ROOT / "assets/GUNE5D/Gauntlet")
        data = args.data or build.cache_path(binary, "GDL_DATA_DIR", devenv.ROOT / "data")
        static_crt = False
        runtime_dirs = args.runtime_dir
        if devenv.WINDOWS and executable.suffix.casefold() == ".exe":
            static_crt = windows_static_crt(binary)
            if not static_crt and not runtime_dirs:
                runtime_dirs = [msvc_runtime_directory()]
        plan = make_plan(executable, assets, data, args.output, tuple(runtime_dirs), static_crt=static_crt)
        if static_crt:
            print("Static CRT imports verified; stale CRT DLLs in package inputs are excluded.")
        total = sum(source.stat().st_size for source, _ in plan.files)
        print(f"{'Would stage' if args.dry_run else 'Staging'} {len(plan.files)} files "
              f"({total:,} bytes) in {plan.output}")
        if args.dry_run:
            for source, destination in plan.files:
                print(f"  {source} -> {destination}")
        else:
            stage(plan)
            print(f"Ready: {plan.output / executable.name}")
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
