import re
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


class WorkflowTest(unittest.TestCase):
    def test_linux_jobs_enable_gateway_sampler_tests(self):
        for filename in ("build.yml", "release.yml"):
            workflow = (ROOT / ".github" / "workflows" / filename).read_text(encoding="utf-8")
            jobs = dict(re.findall(r"^  ([\w-]+):\n(.*?)(?=^  [\w-]+:\n|\Z)", workflow, re.M | re.S))
            expected = ("linux", "clang-tidy-linux") if filename == "build.yml" else ("linux",)
            for name in expected:
                with self.subTest(workflow=filename, job=name):
                    configure = jobs[name].split("      - name: Configure\n", 1)[1].split("\n      - name:", 1)[0]
                    self.assertIn('"-DENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS=ON"', configure)
            for name, job in jobs.items():
                if name not in expected:
                    self.assertNotIn("ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS", job)

    def runtime_scripts(self):
        scripts = []
        for filename in ("build.yml", "release.yml"):
            workflow = (ROOT / ".github" / "workflows" / filename).read_text(encoding="utf-8")
            jobs = dict(re.findall(r"^  ([\w-]+):\n(.*?)(?=^  [\w-]+:\n|\Z)", workflow, re.M | re.S))
            for platform in ("linux", "windows"):
                job = jobs[platform]
                self.assertIn('python-version: "3.12"', job)
                self.assertIn("ctest --test-dir build/RelWithDebInfo --output-on-failure", job)
                self.assertLess(job.index("name: Set up Python"), job.index("name: Locate Python test runtime"))
                self.assertLess(job.index("name: Locate Python test runtime"), job.index("name: Configure"))
                variable = "$env:SPARK_TEST_LIBPYTHON" if platform == "windows" else "$SPARK_TEST_LIBPYTHON"
                configure = job.split("      - name: Configure\n", 1)[1].split("\n      - name:", 1)[0]
                self.assertIn(f'"-DSPARK_TEST_LIBPYTHON:FILEPATH={variable}"', configure)
                block = job.split("      - name: Locate Python test runtime\n", 1)[1].split("\n      - name:", 1)[0]
                self.assertIn("shell: python {0}", block)
                script = "\n".join(line[10:] for line in block.split("        run: |\n", 1)[1].splitlines())
                compile(script, filename, "exec")
                scripts.append(script)
            for name, job in jobs.items():
                if "ctest --test-dir" in job:
                    self.assertIn(name, ("linux", "windows"))
        return scripts

    def test_native_runtime_jobs_pass_selected_python_library(self):
        scripts = self.runtime_scripts()
        self.assertEqual(len(scripts), 4)
        self.assertTrue(all(script == scripts[0] for script in scripts))
        self.assertIn('sysconfig.get_config_var("LDLIBRARY")', scripts[0])
        self.assertIn('sysconfig.get_config_var("LIBDIR")', scripts[0])
        self.assertIn("Path(sys.base_prefix)", scripts[0])
        self.assertIn("runtime.is_file()", scripts[0])
        self.assertIn("library.Py_GetVersion()", scripts[0])
        self.assertIn('version != sys.version.split()[0]', scripts[0])
        self.assertIn('output.write(f"SPARK_TEST_LIBPYTHON={runtime}\\n")', scripts[0])

    def test_native_runtime_discovery_rejects_old_python(self):
        for script in self.runtime_scripts():
            with patch.object(sys, "version_info", (3, 11, 9)):
                with self.assertRaisesRegex(SystemExit, "Python >= 3.12 is required"):
                    exec(script, {})

    def test_build_enforces_project_quality(self):
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertEqual(workflow.count("conanfile.py"), 4)
        self.assertEqual(workflow.count("tools/run_clang_tidy.py"), 2)
        self.assertEqual(workflow.count("--shard ${{ matrix.shard }}"), 2)
        self.assertEqual(workflow.count("shard: [core, native, application, tests-core, selftest]"), 2)
        self.assertEqual(workflow.count("--dry-run --Werror"), 2)
        self.assertEqual(workflow.count("CMAKE_EXPORT_COMPILE_COMMANDS"), 4)
        self.assertEqual(workflow.count("CMAKE_CXX_SCAN_FOR_MODULES:BOOL=OFF"), 2)
        self.assertIn("name: Build & test Linux", workflow)
        self.assertIn("name: Build & test Windows", workflow)
        self.assertNotIn("needs: linux", workflow)
        self.assertNotIn("needs: windows", workflow)
        self.assertEqual(workflow.count("tools/verify_windows_artifacts.ps1"), 1)
        self.assertIn("build/RelWithDebInfo/SHA256SUMS", workflow)

    def test_release_is_published_after_both_artifacts(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertNotIn("gh release upload", workflow)
        self.assertEqual(workflow.count("gh release create"), 1)
        self.assertIn("needs: [release, windows, linux]", workflow)
        self.assertLess(workflow.index("  windows:"), workflow.index("  publish:"))
        self.assertLess(workflow.index("  linux:"), workflow.index("  publish:"))
        self.assertIn("ctest --test-dir build/RelWithDebInfo --output-on-failure", workflow)
        self.assertEqual(workflow.count("tools/verify_windows_artifacts.ps1"), 2)
        self.assertIn("release-windows/SHA256SUMS", workflow)
        self.assertNotIn("SHA256SUMS-windows", workflow)
        self.assertIn("python tests/native/alloc/verify_linux_gateway.py elf release-linux/endstone_spark.so", workflow)
        self.assertNotIn("endstone_spark-linux-x86_64.tar.gz", workflow)
        publish_command = workflow.split("            --notes-file release-notes/release_body.md \\\n", 1)[1]
        assets = [
            line.strip().rstrip("\\").strip()
            for line in publish_command.splitlines()
            if line.startswith("            release-")
        ]
        self.assertEqual(
            assets,
            [
                "release-windows/endstone_spark.dll",
                "release-windows/endstone_spark.pdb",
                "release-linux/endstone_spark.so",
            ],
        )

    def test_windows_version_resource_is_configure_time_generated(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        resource = (ROOT / "src" / "version.rc.in").read_text(encoding="utf-8")
        self.assertIn("configure_file(src/version.rc.in", cmake)
        self.assertIn('target_sources(spark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")', cmake)
        self.assertIn("@PROJECT_VERSION_MAJOR@", resource)
        self.assertIn("@PROJECT_VERSION_MINOR@", resource)
        self.assertIn("@PROJECT_VERSION_PATCH@", resource)
        self.assertIn('VALUE "FileVersion", "@PROJECT_VERSION@.0\\0"', resource)
        self.assertIn('VALUE "ProductVersion", "@PROJECT_VERSION@.0\\0"', resource)
        self.assertNotIn("0.5.3", resource)

    def test_cmake_uses_immutable_official_dependency_defaults(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        expected = {
            "ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY": "https://github.com/EndstoneMC/endstone.git",
            "ENDSTONE_SPARK_ENDSTONE_GIT_TAG": "37b395378d91d6d20f1c52bf9d79dbd20e152458",
            "ENDSTONE_SPARK_PAPI_GIT_REPOSITORY": "https://github.com/EndstoneMC/papi.git",
            "ENDSTONE_SPARK_PAPI_GIT_TAG": "3bc3dbf99010e7af09b967d913b5b6a7e182e989",
        }
        for name, value in expected.items():
            self.assertRegex(
                cmake,
                rf'set\({name} "{re.escape(value)}" CACHE STRING',
            )
        for name in ("ENDSTONE_SPARK_ENDSTONE_GIT_TAG", "ENDSTONE_SPARK_PAPI_GIT_TAG"):
            self.assertRegex(cmake, rf'set\({name} "[0-9a-f]{{40}}" CACHE STRING')
        papi_declaration = cmake.split("FetchContent_Declare(endstone_papi_headers", 1)[1].split(")", 1)[0]
        self.assertNotIn("GIT_SHALLOW", papi_declaration)

if __name__ == "__main__":
    unittest.main()
