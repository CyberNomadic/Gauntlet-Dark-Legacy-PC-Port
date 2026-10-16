"""Windows shipping CRT contracts, evaluated without configuring or building the game."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake/GdlStaticRuntime.cmake"


class StaticRuntimePresetTests(unittest.TestCase):
    def test_only_the_shipping_windows_preset_selects_static_crt(self):
        data = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        presets = {row["name"]: row for row in data["configurePresets"]}
        release = presets["windows-ninja-release"]["cacheVariables"]
        self.assertEqual(release["VCPKG_TARGET_TRIPLET"], "x64-windows-static")
        self.assertEqual(release["CMAKE_MSVC_RUNTIME_LIBRARY"], "MultiThreaded")
        self.assertEqual(release["CMAKE_BUILD_TYPE"], "RelWithDebInfo")
        self.assertEqual(presets["windows-base"]["cacheVariables"]["VCPKG_TARGET_TRIPLET"],
                         "x64-windows")
        for name in ("windows-base", "windows-ninja-debug", "windows-vs2022", "linux-base",
                     "linux-ninja-debug", "linux-ninja-release"):
            self.assertNotIn("CMAKE_MSVC_RUNTIME_LIBRARY", presets[name].get("cacheVariables", {}))


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for script-only contract checks")
class StaticRuntimeCmakeTests(unittest.TestCase):
    def run_script(self, setup="", suffix="", environment=None):
        parent = ROOT / "build"
        with tempfile.TemporaryDirectory(prefix="static-runtime-contract-",
                                         dir=parent if parent.is_dir() else None) as temporary:
            script = pathlib.Path(temporary) / "contract.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.30)\n'
                f'include("{HELPER.as_posix()}")\n'
                'set(CMAKE_HOST_WIN32 TRUE)\n'
                'set(CMAKE_GENERATOR Ninja)\n'
                'set(CMAKE_BUILD_TYPE RelWithDebInfo)\n'
                'set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)\n'
                'set(VCPKG_TARGET_TRIPLET x64-windows-static)\n'
                'set(MSVC TRUE)\n' + setup + '\n'
                'gdl_configure_windows_runtime()\n'
                'gdl_validate_windows_runtime()\n'
                'message("STATIC=${GDL_WINDOWS_STATIC_RUNTIME}")\n' + suffix,
                encoding="utf-8")
            env = os.environ.copy()
            for name in ("CL", "_CL_", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "CFLAGS",
                         "TARGET_CFLAGS", "CFLAGS_x86_64_pc_windows_msvc",
                         "CFLAGS_x86_64-pc-windows-msvc", "CXXFLAGS", "TARGET_CXXFLAGS",
                         "CXXFLAGS_x86_64_pc_windows_msvc", "CXXFLAGS_x86_64-pc-windows-msvc"):
                env.pop(name, None)
            env.update(environment or {})
            return subprocess.run([shutil.which("cmake"), "-P", str(script)], cwd=temporary,
                                  env=env, capture_output=True, text=True, check=False)

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_diagnostic(self, result, expected):
        # CMake wraps diagnostics to its output width, independently of semantics.
        self.assertIn(expected, " ".join(result.stderr.split()), result.stderr)

    def test_release_contract_uses_the_pinned_runtime(self):
        for build_type in ("Release", "RelWithDebInfo"):
            with self.subTest(build_type=build_type):
                result = self.run_script(f"set(CMAKE_BUILD_TYPE {build_type})")
                self.assert_success(result)
                self.assertIn("STATIC=ON", result.stderr)

    def test_mixed_runtime_or_dependency_triplet_is_rejected(self):
        for setup in ("set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)",
                      "set(VCPKG_TARGET_TRIPLET x64-windows)"):
            with self.subTest(setup=setup):
                result = self.run_script(setup)
                self.assertNotEqual(result.returncode, 0)
                self.assert_diagnostic(result, "static Windows shipping profile requires")

    def test_debug_and_multi_config_static_profiles_fail_explicitly(self):
        for setup in ("set(CMAKE_BUILD_TYPE Debug)",
                      'set(CMAKE_GENERATOR "Ninja Multi-Config")',
                      'set(CMAKE_CONFIGURATION_TYPES "Release;Debug")'):
            with self.subTest(setup=setup):
                result = self.run_script(setup)
                self.assertNotEqual(result.returncode, 0)
                self.assert_diagnostic(result, "single-config Release/RelWithDebInfo only")

    def test_dynamic_debug_development_and_linux_are_unchanged(self):
        setups = (
            'set(CMAKE_BUILD_TYPE Debug)\nset(VCPKG_TARGET_TRIPLET x64-windows)\n'
            'unset(CMAKE_MSVC_RUNTIME_LIBRARY)',
            'set(CMAKE_GENERATOR "Visual Studio 17 2022")\n'
            'set(VCPKG_TARGET_TRIPLET x64-windows)\n'
            'set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")',
            'set(CMAKE_HOST_WIN32 FALSE)\nset(MSVC FALSE)\n'
            'set(VCPKG_TARGET_TRIPLET x64-linux)\nunset(CMAKE_MSVC_RUNTIME_LIBRARY)',
        )
        for setup in setups:
            with self.subTest(setup=setup):
                result = self.run_script(setup)
                self.assert_success(result)
                self.assertNotIn("STATIC=ON", result.stderr)

    def test_dynamic_crt_flag_overrides_are_rejected(self):
        for setup, environment in (
            ('set(CMAKE_CXX_FLAGS "/O2 /MD")', {}),
            ('set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-MDd")', {}),
            ('set(CMAKE_CXX_FLAGS_RELEASE "/MTd")', {}),
            ("", {"CL": "/MD"}),
            ("", {"_CL_": " /MDd /Zi"}),
            ("", {"CXXFLAGS": "/MTd"}),
        ):
            with self.subTest(setup=setup, environment=environment):
                result = self.run_script(setup, environment=environment)
                self.assertNotEqual(result.returncode, 0)
                self.assert_diagnostic(result, "overrides the pinned static CRT")

    def rust_flags(self, environment):
        return self.run_script(suffix=(
            'gdl_static_rust_flags(flags)\nstring(ASCII 31 separator)\n'
            'string(REPLACE "${separator}" "<ARG>" displayed "${flags}")\n'
            'message("RUST=${displayed}")\n'), environment=environment)

    def test_rust_target_flags_preserve_effective_environment_and_pin_crt(self):
        for environment, expected in (
            ({}, "-C<ARG>target-feature=+crt-static"),
            ({"RUSTFLAGS": "-C debuginfo=1"},
             "-C<ARG>debuginfo=1<ARG>-C<ARG>target-feature=+crt-static"),
            ({"RUSTFLAGS": "ignored", "CARGO_ENCODED_RUSTFLAGS": "--cfg\x1fpinned_runtime"},
             "--cfg<ARG>pinned_runtime<ARG>-C<ARG>target-feature=+crt-static"),
            ({"RUSTFLAGS": "ignored", "CARGO_ENCODED_RUSTFLAGS": ""},
             "-C<ARG>target-feature=+crt-static"),
        ):
            with self.subTest(environment=environment):
                result = self.rust_flags(environment)
                self.assert_success(result)
                self.assertIn("RUST=" + expected, result.stderr)

    def test_rust_dynamic_crt_override_is_rejected(self):
        for environment in ({"RUSTFLAGS": "-C target-feature=-crt-static"},
                            {"CARGO_ENCODED_RUSTFLAGS": "-C\x1ftarget-feature=-crt-static"}):
            with self.subTest(environment=environment):
                result = self.rust_flags(environment)
                self.assertNotEqual(result.returncode, 0)
                self.assert_diagnostic(result, "explicitly disable the pinned static CRT")

    def test_rust_c_dependencies_cannot_override_the_crt(self):
        for name in ("CFLAGS", "TARGET_CFLAGS", "CFLAGS_x86_64_pc_windows_msvc",
                     "CFLAGS_x86_64-pc-windows-msvc", "TARGET_CXXFLAGS",
                     "CXXFLAGS_x86_64_pc_windows_msvc", "CXXFLAGS_x86_64-pc-windows-msvc"):
            for flag in ("/MD", "/MDd", "/MTd"):
                with self.subTest(name=name, flag=flag):
                    result = self.rust_flags({name: flag})
                    self.assertNotEqual(result.returncode, 0)
                    self.assert_diagnostic(result, "overrides the Rust bridge's release static CRT")


if __name__ == "__main__":
    unittest.main()
