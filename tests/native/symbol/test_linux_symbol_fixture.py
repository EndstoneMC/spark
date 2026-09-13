import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def parse_output(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key] = value
    return values


def run_fixture(executable: Path, strip_tool: Path) -> dict[str, str]:
    with tempfile.TemporaryDirectory(prefix="spark-symbol-fixture-") as directory:
        copy = Path(directory) / executable.name
        shutil.copy2(executable, copy)
        subprocess.run([str(strip_tool), "--strip-all", str(copy)], check=True)
        readelf = shutil.which("readelf")
        if readelf is not None:
            sections = subprocess.run([readelf, "-S", str(copy)], check=True, capture_output=True, text=True).stdout
            assert ".symtab" not in sections
            assert ".debug_" not in sections
            dynamic = subprocess.run([readelf, "-d", str(copy)], check=True, capture_output=True, text=True).stdout
            assert "libc++.so" not in dynamic
            assert "libc++abi.so" not in dynamic
            symbols = subprocess.run([readelf, "-Ws", str(copy)], check=True, capture_output=True, text=True).stdout
            for name in (
                "_ZTVN10__cxxabiv117__class_type_infoE",
                "_ZTVN10__cxxabiv120__si_class_type_infoE",
                "_ZTVN10__cxxabiv121__vmi_class_type_infoE",
            ):
                assert not any(name in line and " UND " in line for line in symbols.splitlines())
        completed = subprocess.run([str(copy)], check=True, capture_output=True, text=True)
    return parse_output(completed.stdout)


def number(values: dict[str, str], key: str) -> int:
    return int(values[key], 10)


def assert_same_results(first: dict[str, str], second: dict[str, str]) -> None:
    for key, value in first.items():
        if key.endswith("_label") or key.endswith("_root"):
            assert second[key] == value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--strip-tool", required=True)
    args = parser.parse_args()
    executable = Path(args.executable)
    strip_tool = Path(args.strip_tool)
    assert strip_tool.is_file()

    values = run_fixture(executable, strip_tool)
    second_launch = run_fixture(executable, strip_tool)
    assert_same_results(values, second_launch)

    # Query composition and order are independent; shared and prefixed references stay suppressed alone or together.
    assert values["shared_single_label"] == ""
    assert values["prefixed_single_label"] == ""
    assert values["shared_pair_label"] == ""
    assert values["prefixed_pair_label"] == ""
    assert values["shared_unique_label"] == ""
    assert "late_label" in values
    assert values["late_label"].startswith("str?: Level - tick late")

    # Positive unique strong string, embedded bytes cleared, and all incomplete/global ambiguity cases suppressed.
    assert values["unique_label"].startswith("str?: Level - tick unique")
    assert values["owner_indirect_label"].startswith("str?: Level - tick malformed")
    assert values["call_tail_label"].startswith("str?: Level - tick call tail")
    assert values["embedded_label"].startswith("str?: Level - tick embedded")
    for key in (
        "truncated",
        "other_indirect",
        "truncated_call",
        "other_call",
        "syscall",
        "unreachable",
        "overlap",
        "unindexed",
        "budget",
        "large",
        "function_budget",
        "batch_budget",
    ):
        assert values[f"{key}_label"] == ""

    # Weak accumulation is useful only when every contributing string is unique and complete.
    assert values["weak_label"].startswith("str?: ")
    assert "(+2 more)" in values["weak_label"]
    assert values["weak_shared_label"] == ""
    assert values["weak_other_label"] == ""
    assert values["weak_ambiguous_label"] == ""

    # Strong vtable evidence survives; interior vtable entries and interior thunk destinations do not.
    assert values["vtable_label"].startswith("vtable: fixture::VtableOwner::vfn[0]")
    assert values["thunk_label"] == ""

    # The diagnostic oracle proves each conservative validator branch was exercised.
    assert number(values, "cache_string_validation_budget_exhausted") >= 1
    assert number(values, "shared_string_reference_shared") >= 1
    assert number(values, "shared_string_reference_terminal_hits_skipped") >= 1000
    assert number(values, "shared_string_validation_budget_exhausted") == 0
    assert number(values, "shared_string_instruction_budget_exhausted") == 0
    assert number(values, "unindexed_string_reference_terminal_hits_skipped") >= 1000
    assert number(values, "unindexed_string_validation_functions") <= 2
    assert number(values, "large_string_function_byte_budget_exhausted") >= 1
    assert number(values, "function_string_function_instruction_budget_exhausted") >= 1
    assert number(values, "batch_string_instruction_budget_exhausted") >= 1
    assert number(values, "embedded_string_reference_exact_hits") >= 1
    assert number(values, "embedded_string_reference_interior_rejections") >= 1
    assert number(values, "unreachable_string_reference_ambiguities") >= 1
    assert number(values, "unreachable_string_reference_unreachable") >= 1
    assert number(values, "overlap_string_reference_ambiguities") >= 1
    assert number(values, "overlap_string_reference_overlaps") >= 1
    assert number(values, "unindexed_string_reference_ambiguities") >= 1
    assert number(values, "unindexed_string_reference_unindexed") >= 1
    assert number(values, "cache_vtable_interior_target_rejections") >= 1
    assert number(values, "thunk_thunk_interior_destination_rejections") >= 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
