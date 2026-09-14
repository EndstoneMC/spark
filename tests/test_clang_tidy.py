import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import run_clang_tidy


def entry(root: pathlib.Path, relative: str, output: str) -> dict[str, str]:
    return {"file": str(root / pathlib.PurePosixPath(relative)), "output": output}


class ClangTidyDriverTest(unittest.TestCase):
    def test_extract_target_accepts_windows_and_posix_output_paths(self) -> None:
        self.assertEqual(
            run_clang_tidy.extract_target(r"C:\build\CMakeFiles\spark_native.dir\src\native\sampler.cpp.obj"),
            "spark_native",
        )
        self.assertEqual(
            run_clang_tidy.extract_target("/build/CMakeFiles/spark_core.dir/src/core/profiler.cpp.o"),
            "spark_core",
        )
        self.assertIsNone(run_clang_tidy.extract_target("/build/compile_commands.json"))

    def test_shards_are_complete_disjoint_and_order_independent(self) -> None:
        database = [
            entry(ROOT, "src/core/shared.cpp", r"CMakeFiles\spark_native.dir\src\core\shared.cpp.obj"),
            entry(ROOT, "src/core/shared.cpp", "/build/CMakeFiles/spark_core.dir/src/core/shared.cpp.o"),
            entry(ROOT, "src/native/sampler.cpp", "/build/CMakeFiles/spark_native.dir/src/native/sampler.cpp.o"),
            entry(
                ROOT,
                "src/application/service.cpp",
                "/build/CMakeFiles/spark_application.dir/src/application/service.cpp.o",
            ),
            entry(ROOT, "src/platform/endstone.cpp", "/build/CMakeFiles/spark.dir/src/platform/endstone.cpp.o"),
            entry(ROOT, "src/plugin.cpp", "/build/CMakeFiles/spark.dir/src/plugin.cpp.o"),
            entry(
                ROOT,
                "tests/native/sampler_test.cpp",
                "/build/CMakeFiles/spark_core_test.dir/tests/native/sampler_test.cpp.o",
            ),
            entry(
                ROOT,
                "tests/application/service_test.cpp",
                "/build/CMakeFiles/spark_application_test.dir/tests/application/service_test.cpp.o",
            ),
            entry(
                ROOT,
                "tests/platform/papi_test.cpp",
                "/build/CMakeFiles/spark_papi_test.dir/tests/platform/papi_test.cpp.o",
            ),
            entry(
                ROOT,
                "tests/core/profiler_test.cpp",
                "/build/CMakeFiles/spark_core_test.dir/tests/core/profiler_test.cpp.o",
            ),
            entry(
                ROOT,
                "tests/proto/proto_test.cpp",
                "/build/CMakeFiles/spark_proto_test.dir/tests/proto/proto_test.cpp.o",
            ),
            entry(
                ROOT,
                "tests/net/websocket_test.cpp",
                "/build/CMakeFiles/spark_net_test.dir/tests/net/websocket_test.cpp.o",
            ),
            entry(ROOT, "tests/selftest.cpp", "/build/CMakeFiles/spark_selftest.dir/tests/selftest.cpp.o"),
            entry(ROOT, "tests/tools_fixture.cpp", "/build/CMakeFiles/spark_tool_test.dir/tests/tools_fixture.cpp.o"),
            entry(ROOT, "third_party/not_owned.cpp", "/build/CMakeFiles/other.dir/third_party/not_owned.cpp.o"),
        ]

        matrix = run_clang_tidy.source_shards(database, ROOT)
        all_sources = set().union(*matrix.values())
        expected = {pathlib.Path(item["file"]).resolve() for item in database[:-1]}
        self.assertEqual(all_sources, expected)
        self.assertEqual(set(matrix), set(run_clang_tidy.SHARDS))
        self.assertTrue(all(matrix.values()))
        for index, shard in enumerate(run_clang_tidy.SHARDS):
            for other in run_clang_tidy.SHARDS[index + 1 :]:
                self.assertTrue(matrix[shard].isdisjoint(matrix[other]))

        reversed_matrix = run_clang_tidy.source_shards(list(reversed(database)), ROOT)
        self.assertEqual(matrix, reversed_matrix)

    def test_unknown_source_requires_a_single_known_target(self) -> None:
        database = [
            entry(ROOT, "src/new_area.cpp", "/build/CMakeFiles/spark_core.dir/src/new_area.cpp.o"),
            entry(ROOT, "src/new_area.cpp", "/build/CMakeFiles/spark_native.dir/src/new_area.cpp.o"),
        ]
        with self.assertRaisesRegex(ValueError, "conflicting shard targets"):
            run_clang_tidy.source_shards(database, ROOT)

    def test_match_and_named_shard_selectors(self) -> None:
        database = [
            entry(ROOT, "src/core/core.cpp", "/build/CMakeFiles/spark_core.dir/src/core/core.cpp.o"),
            entry(ROOT, "src/native/native.cpp", "/build/CMakeFiles/spark_native.dir/src/native/native.cpp.o"),
            entry(ROOT, "tests/selftest.cpp", "/build/CMakeFiles/spark_selftest.dir/tests/selftest.cpp.o"),
        ]
        matrix = run_clang_tidy.source_shards(database, ROOT, r"^(src/core|tests/selftest)")
        self.assertEqual(len(matrix["core"]), 1)
        self.assertEqual(len(matrix["selftest"]), 1)
        self.assertFalse(matrix["native"])

    def test_selects_only_c_cpp_translation_units(self) -> None:
        database = [
            entry(ROOT, "src/core/core.c", "/build/CMakeFiles/spark_core.dir/src/core/core.c.o"),
            entry(ROOT, "src/core/core.cc", "/build/CMakeFiles/spark_core.dir/src/core/core.cc.o"),
            entry(ROOT, "src/core/core.cpp", "/build/CMakeFiles/spark_core.dir/src/core/core.cpp.o"),
            entry(ROOT, "src/core/core.cxx", "/build/CMakeFiles/spark_core.dir/src/core/core.cxx.o"),
            entry(ROOT, "src/core/core.c++", "/build/CMakeFiles/spark_core.dir/src/core/core.c++.o"),
            entry(ROOT, "src/core/core.C", "/build/CMakeFiles/spark_core.dir/src/core/core.C.o"),
            entry(
                ROOT,
                "tests/native/sampler_test.cpp",
                "/build/CMakeFiles/spark_native.dir/tests/native/sampler_test.cpp.o",
            ),
            entry(
                ROOT,
                "tests/native/symbol/symbol_guess_linux_fixture.S",
                "/build/CMakeFiles/spark_native.dir/tests/native/symbol/symbol_guess_linux_fixture.S.o",
            ),
            entry(ROOT, "src/native/fixture.s", "/build/CMakeFiles/spark_native.dir/src/native/fixture.s.o"),
            entry(ROOT, "tests/core/fixture.asm", "/build/CMakeFiles/spark_core.dir/tests/core/fixture.asm.o"),
            entry(ROOT, "src/core/fixture.h", "/build/CMakeFiles/spark_core.dir/src/core/fixture.h.o"),
            entry(ROOT, "tests/core/fixture.txt", "/build/CMakeFiles/spark_core.dir/tests/core/fixture.txt.o"),
        ]

        matrix = run_clang_tidy.source_shards(database, ROOT)
        self.assertEqual(
            matrix["core"],
            {
                ROOT / "src/core/core.c",
                ROOT / "src/core/core.cc",
                ROOT / "src/core/core.cpp",
                ROOT / "src/core/core.cxx",
                ROOT / "src/core/core.c++",
                ROOT / "src/core/core.C",
            },
        )
        self.assertEqual(matrix["native"], {ROOT / "tests/native/sampler_test.cpp"})
        self.assertEqual(set().union(*matrix.values()), set(matrix["core"]) | set(matrix["native"]))


if __name__ == "__main__":
    unittest.main()
