"""Packaging copies original files unchanged and cannot overwrite its inputs or an old package."""
import contextlib
import io
import pathlib
import stat
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import package


def pe_image(imports=(), delayed=(), delay_va=False):
    """Minimal independent PE32+ import fixture; no executable code is needed."""
    image = bytearray(4096)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x22)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x20b)
    image_base = 0x10000000 if delay_va else 0x140000000
    struct.pack_into("<Q", image, optional + 24, image_base)
    struct.pack_into("<I", image, optional + 60, 0x200)
    struct.pack_into("<I", image, optional + 108, 16)
    section = optional + 240
    image[section:section + 8] = b".idata\0\0"
    struct.pack_into("<IIII", image, section + 8, 0xe00, 0x1000, 0xe00, 0x200)
    names = 0x800
    for directory, raw, stride, libraries in ((1, 0x200, 20, imports), (13, 0x500, 32, delayed)):
        if not libraries:
            continue
        struct.pack_into("<II", image, optional + 112 + directory * 8,
                         0x1000 + raw - 0x200, (len(libraries) + 1) * stride)
        for index, library in enumerate(libraries):
            address = 0x1000 + names - 0x200
            if directory == 13:
                struct.pack_into("<8I", image, raw + index * stride,
                                 0 if delay_va else 1, address + (image_base if delay_va else 0),
                                 0, 0, 0, 0, 0, 0)
            else:
                struct.pack_into("<5I", image, raw + index * stride, 0, 0, 0, address, 0)
            encoded = library.encode("ascii") + b"\0"
            image[names:names + len(encoded)] = encoded
            names += len(encoded)
    return image


class PackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl package ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.executable = self.root / "build/bin/gauntlet.exe"
        self.assets = self.root / "original/gAuNtLeT"
        self.data = self.root / "defaults"
        self.output = self.root / "distribution/game"
        for name, content in {
            "build/bin/gauntlet.exe": b"executable",
            "build/bin/glfw3.dll": b"runtime",
            "build/bin/tests.exe": b"not shipped",
            "build/bin/shaders/immediate.vert.spv": b"vertex shader",
            "build/bin/shaders/immediate.frag.spv": b"fragment shader",
            "build/bin/shaders/postprocess.vert.spv": b"postprocess vertex shader",
            "build/bin/shaders/depth_of_field.frag.spv": b"depth of field shader",
            "build/bin/shaders/depth_of_field_ms.frag.spv": b"multisampled depth shader",
            "build/bin/shaders/bloom.frag.spv": b"bloom and heat shader",
            "build/bin/shaders/bloom_ms.frag.spv": b"multisampled bloom and heat shader",
            "build/bin/shaders/ambient_occlusion.frag.spv": b"contact shading shader",
            "build/bin/shaders/ambient_occlusion_ms.frag.spv": b"multisampled contact shader",
            "original/gAuNtLeT/PDATA/WAR.WAD": b"original player table\x00\xff",
            "original/gAuNtLeT/unknown/file.bin": b"preserve unrecognized files",
            "original/CARDDEMO/icon.tpl": b"original icon",
            "defaults/config.json": b"{}",
            "defaults/text/en.json": b"{}",
        }.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        (self.assets / "EMPTY").mkdir()

    def plan(self, output=None, runtime_dirs=()):
        return package.make_plan(self.executable, self.assets, self.data,
                                  output or self.output, runtime_dirs)

    def test_every_optional_effect_shader_is_required_before_packaging(self):
        for name in ("bloom.frag.spv", "bloom_ms.frag.spv",
                     "ambient_occlusion.frag.spv", "ambient_occlusion_ms.frag.spv"):
            with self.subTest(shader=name):
                path = self.executable.parent / "shaders" / name
                contents = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ValueError, name):
                    self.plan()
                self.assertFalse(self.output.exists())
                path.write_bytes(contents)

    def test_stages_original_tree_bytes_casing_empty_directories_and_runtime(self):
        plan = self.plan()
        originals = {source: source.read_bytes() for source, _ in plan.files}
        package.stage(plan)
        for source, relative in plan.files:
            self.assertEqual((self.output / relative).read_bytes(), originals[source])
            self.assertEqual(source.read_bytes(), originals[source])
        self.assertTrue((self.output / "gAuNtLeT/EMPTY").is_dir())
        self.assertTrue((self.output / "CARDDEMO/icon.tpl").is_file())
        self.assertTrue((self.output / "glfw3.dll").is_file())
        self.assertFalse((self.output / "tests.exe").exists())
        self.assertFalse((self.output / "unpacked").exists())

    def test_planning_does_not_create_output(self):
        self.assertGreater(len(self.plan().files), 0)
        self.assertFalse(self.output.exists())

    def test_existing_outputs_are_refused_without_changing_them(self):
        self.output.mkdir(parents=True)
        marker = self.output / "save.json"
        marker.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.plan()
        self.assertEqual(marker.read_bytes(), b"keep")

    def test_outputs_inside_source_trees_are_refused(self):
        for parent in (self.assets, self.assets.parent, self.data, self.executable.parent):
            with self.subTest(parent=parent), self.assertRaisesRegex(ValueError, "inside"):
                self.plan(parent / "new-package")

    def test_stage_rechecks_that_output_did_not_appear_since_planning(self):
        plan = self.plan()
        self.output.mkdir(parents=True)
        with self.assertRaises(FileExistsError):
            package.stage(plan)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_missing_inputs_are_rejected_before_writing(self):
        for relative in ("CARDDEMO", "../build/bin/shaders/immediate.vert.spv",
                         "../defaults/config.json"):
            path = self.assets.parent / relative
            renamed = path.with_name(path.name + "-held")
            path.rename(renamed)
            try:
                with self.assertRaises(ValueError):
                    self.plan()
                self.assertFalse(self.output.exists())
            finally:
                renamed.rename(path)

    def test_extra_runtime_libraries_and_conflicts(self):
        runtime = self.root / "CRT"
        runtime.mkdir()
        (runtime / "msvcp140.dll").write_bytes(b"CRT")
        plan = self.plan(runtime_dirs=(runtime,))
        self.assertIn(pathlib.Path("msvcp140.dll"), [dest for _, dest in plan.files])
        (runtime / "GLFW3.DLL").write_bytes(b"wrong runtime")
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.plan(runtime_dirs=(runtime,))

    def test_dry_run_validates_and_reports_without_copying(self):
        with mock.patch.object(package.devenv, "WINDOWS", False), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(package.main([
                "--executable", str(self.executable), "--assets", str(self.assets),
                "--data", str(self.data), "--output", str(self.output), "--dry-run"]), 0)
        self.assertIn("Would stage", output.getvalue())
        self.assertIn("WAR.WAD", output.getvalue())
        self.assertFalse(self.output.exists())

    def test_linked_entries_in_asset_trees_are_refused(self):
        linked = self.assets / "unknown"
        original_lstat = pathlib.Path.lstat

        def lstat(path):
            if path == linked:
                return SimpleNamespace(st_mode=stat.S_IFLNK)
            return original_lstat(path)

        with mock.patch.object(pathlib.Path, "lstat", lstat), \
                self.assertRaisesRegex(ValueError, "Linked entry"):
            self.plan()
        self.assertFalse(self.output.exists())

    def test_windows_junction_is_rejected_without_pathlib_is_junction(self):
        junction = self.assets / "unknown"
        original_lstat = pathlib.Path.lstat

        def lstat(path):
            if path == junction:
                return SimpleNamespace(st_mode=stat.S_IFDIR,
                                       st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)
            return original_lstat(path)

        # A junction is a directory, not a symlink; pre-3.12 pathlib has no
        # is_junction method. The lstat reparse flag must stop traversal itself.
        with mock.patch.object(pathlib.Path, "lstat", lstat), \
                self.assertRaisesRegex(ValueError, "Linked entry"):
            self.plan()
        self.assertFalse(self.output.exists())

    def test_ordinary_file_and_directory_are_not_links(self):
        self.assertFalse(package.linked_entry(self.assets))
        self.assertFalse(package.linked_entry(self.assets / "PDATA/WAR.WAD"))

    def test_msvc_crt_comes_from_the_developer_redist_directory(self):
        redist = self.root / "redist"
        crt = redist / "x64/Microsoft.VC143.CRT"
        crt.mkdir(parents=True)
        with mock.patch.object(package.devenv, "environment",
                               return_value={"VCToolsRedistDir": str(redist)}):
            self.assertEqual(package.msvc_runtime_directory(), crt)
        with mock.patch.object(package.devenv, "environment", return_value={}), \
                self.assertRaisesRegex(ValueError, "--runtime-dir"):
            package.msvc_runtime_directory()

    def test_static_game_import_audit_checks_direct_and_delayed_crt_families(self):
        for library in ("VCRUNTIME140.dll", "vcruntime140_1d.dll", "MSVCP140.dll", "msvcp140_atomic_wait.dll",
                        "ucrtbase.dll", "ucrtbased.dll", "api-ms-win-crt-math-l1-1-0.dll",
                        "msvcrt.dll", "msvcr120.dll", "concrt140.dll"):
            for delayed in (False, True):
                with self.subTest(library=library, delayed=delayed):
                    self.executable.write_bytes(pe_image(
                        ("KERNEL32.dll",) if delayed else ("KERNEL32.dll", library),
                        (library,) if delayed else ()))
                    with self.assertRaisesRegex(ValueError, "static CRT.*dynamic imports"):
                        package.make_plan(self.executable, self.assets, self.data,
                                          self.output, static_crt=True)
                    self.assertFalse(self.output.exists())

    def test_pe_imports_accept_both_rva_and_legacy_va_delay_descriptors(self):
        for va in (False, True):
            self.executable.write_bytes(pe_image(("KERNEL32.dll",), ("VULKAN-1.dll",), va))
            self.assertEqual(package.pe_imports(self.executable), ("kernel32.dll", "vulkan-1.dll"))
            package.require_static_crt(self.executable)

    def test_malformed_or_truncated_pe_does_not_certify_static_runtime(self):
        fixtures = [b"not a PE", pe_image()[:63]]
        image = pe_image(("KERNEL32.dll",))
        struct.pack_into("<I", image, 0x98 + 112 + 8, 0x7fffffff)
        fixtures.append(image)
        image = pe_image(("KERNEL32.dll",))
        struct.pack_into("<I", image, 0x98 + 112 + 8 + 4, 20) # omit descriptor terminator
        fixtures.append(image)
        image = pe_image(delayed=("KERNEL32.dll",))
        struct.pack_into("<I", image, 0x500, 2) # undefined delay attributes
        fixtures.append(image)
        for image in fixtures:
            with self.subTest(size=len(image)):
                self.executable.write_bytes(image)
                with self.assertRaises(ValueError):
                    package.require_static_crt(self.executable)

    def test_static_plan_excludes_stale_crt_but_keeps_explicit_non_crt_dependencies(self):
        self.executable.write_bytes(pe_image(("KERNEL32.dll", "VULKAN-1.dll")))
        stale = self.executable.parent / "vcruntime140.dll"
        stale.write_bytes(b"old dynamic game CRT")
        extra = self.root / "runtime"
        extra.mkdir()
        (extra / "MSVCP140.dll").write_bytes(b"unused CRT")
        (extra / "openal32.dll").write_bytes(b"explicit dependency")
        original = {path: path.read_bytes() for path in (stale, extra / "MSVCP140.dll")}
        plan = package.make_plan(self.executable, self.assets, self.data, self.output,
                                 (extra,), static_crt=True)
        names = {destination.name.casefold() for _, destination in plan.files}
        self.assertNotIn("vcruntime140.dll", names)
        self.assertNotIn("msvcp140.dll", names)
        self.assertIn("openal32.dll", names)
        self.assertIn("glfw3.dll", names) # do not guess which non-CRT dependencies the caller needs
        for path, content in original.items():
            self.assertEqual(path.read_bytes(), content)

    def test_runtime_mode_comes_from_cache_and_rejects_mixed_contracts(self):
        binary = self.root / "configured"
        binary.mkdir()
        cache = binary / "CMakeCache.txt"
        with self.assertRaisesRegex(ValueError, "Configure"):
            package.windows_static_crt(binary)
        for triplet, runtime, expected in (
                ("x64-windows-static", "MultiThreaded", True),
                ("x64-windows", "MultiThreadedDLL", False),
                ("x64-windows", "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL", False),
                ("x64-windows", "", False)):
            for triplet_type in ("STRING", "UNINITIALIZED"):
                for runtime_type in ("STRING", "UNINITIALIZED"):
                    with self.subTest(triplet=triplet, runtime=runtime,
                                      triplet_type=triplet_type, runtime_type=runtime_type):
                        cache.write_text(f"VCPKG_TARGET_TRIPLET:{triplet_type}={triplet}\n"
                                         f"CMAKE_MSVC_RUNTIME_LIBRARY:{runtime_type}={runtime}\n", encoding="utf-8")
                        self.assertIs(package.windows_static_crt(binary), expected)
        for triplet, runtime in (("x64-windows", "MultiThreaded"),
                                 ("x64-windows-static", "MultiThreadedDLL"),
                                 ("x64-windows-static", "MultiThreadedDebug")):
            for runtime_type in ("STRING", "UNINITIALIZED"):
                with self.subTest(triplet=triplet, runtime=runtime, runtime_type=runtime_type):
                    cache.write_text(f"VCPKG_TARGET_TRIPLET:STRING={triplet}\n"
                                     f"CMAKE_MSVC_RUNTIME_LIBRARY:{runtime_type}={runtime}\n", encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "mixed Windows runtime"):
                        package.windows_static_crt(binary)

    def test_static_cli_does_not_discover_or_require_developer_crt(self):
        self.executable.write_bytes(pe_image(("KERNEL32.dll",)))
        with mock.patch.object(package.devenv, "WINDOWS", True), \
                mock.patch.object(package, "windows_static_crt", return_value=True), \
                mock.patch.object(package, "msvc_runtime_directory") as redist, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(package.main([
                "--executable", str(self.executable), "--assets", str(self.assets),
                "--data", str(self.data), "--output", str(self.output), "--dry-run"]), 0)
        redist.assert_not_called()
        self.assertIn("Static CRT imports verified", output.getvalue())

    def test_dynamic_development_cli_keeps_redist_discovery(self):
        redist_path = self.root / "redist"
        redist_path.mkdir()
        (redist_path / "vcruntime140.dll").write_bytes(b"development runtime")
        with mock.patch.object(package.devenv, "WINDOWS", True), \
                mock.patch.object(package, "windows_static_crt", return_value=False), \
                mock.patch.object(package, "msvc_runtime_directory", return_value=redist_path) as redist, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(package.main([
                "--executable", str(self.executable), "--assets", str(self.assets),
                "--data", str(self.data), "--output", str(self.output), "--dry-run"]), 0)
        redist.assert_called_once_with()
        self.assertIn("vcruntime140.dll", output.getvalue())


if __name__ == "__main__":
    unittest.main()
