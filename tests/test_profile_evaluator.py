#!/usr/bin/env python3

import gzip
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import profile_evaluator as evaluator  # noqa: E402


def varint(value: int) -> bytes:
    output = bytearray()
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)
    return bytes(output)


def field(number: int, wire_type: int, payload: bytes | int) -> bytes:
    output = bytearray(varint((number << 3) | wire_type))
    if wire_type == 0:
        output.extend(varint(payload))
    elif wire_type == 1:
        output.extend(int(payload).to_bytes(8, "little"))
    elif wire_type == 2:
        output.extend(varint(len(payload)))
        output.extend(payload)
    else:
        raise AssertionError(f"unsupported test wire type: {wire_type}")
    return bytes(output)


def text(number: int, value: str) -> bytes:
    return field(number, 2, value.encode())


def message(number: int, value: bytes) -> bytes:
    return field(number, 2, value)


def packed_doubles(number: int, values: list[float]) -> bytes:
    return field(number, 2, b"".join(struct.pack("<d", value) for value in values))


def packed_int32(number: int, values: list[int]) -> bytes:
    return field(number, 2, b"".join(varint(value) for value in values))


def node(
    module: str,
    method_name: str,
    total: float,
    children: list[int] | None = None,
    sampled_rva: int | None = None,
    function_rva: int | None = None,
) -> bytes:
    result = text(3, module) + text(4, method_name) + packed_doubles(8, [total])
    if children:
        result += packed_int32(9, children)
    if sampled_rva is not None:
        result += field(1001, 0, sampled_rva)
    if function_rva is not None:
        result += field(1002, 0, function_rva)
    return result


def thread(name: str, nodes: list[bytes], total: float, roots: list[int]) -> bytes:
    result = text(1, name)
    result += b"".join(message(3, value) for value in nodes)
    result += packed_doubles(4, [total])
    result += packed_int32(5, roots)
    return result


def metadata_entry(key: str, value: str) -> bytes:
    return message(14, text(1, key) + text(2, value))


def synthetic_profile() -> bytes:
    metadata = field(15, 0, 0)
    metadata += metadata_entry("Symbol guess function ranges", "123")
    metadata += metadata_entry("Symbol guess batch microseconds", "45")

    server_nodes = [
        node("fixture_server.exe", "0x1000", 40.0),
        node("kernel32.dll", "Sleep", 10.0),
        node(
            "fixture_server.exe",
            "0x2000 (vtable: Level::vfn[3])",
            100.0,
            [0, 1],
        ),
    ]
    worker_nodes = [
        node("fixture_server.exe", "0x3000 (str?: chunk worker task)", 30.0),
        node("fixture_server.exe", "Level::tick", 50.0, [0]),
    ]
    # Unknown fields ensure the hand-written wire reader safely skips fields it
    # does not need from the full SamplerData schema.
    return (
        message(1, metadata)
        + message(2, thread("Server thread", server_nodes, 100.0, [2]))
        + message(2, thread("Worker", worker_nodes, 50.0, [1]))
        + field(99, 0, 7)
    )


class ProfileEvaluatorTest(unittest.TestCase):
    def test_synthetic_profile_metrics(self) -> None:
        profile = evaluator.parse_profile(synthetic_profile())
        report = evaluator.evaluate_profile(profile, top=10)

        self.assertEqual(report["weight_unit"], "milliseconds")
        self.assertEqual(report["threads"], 2)
        self.assertEqual(report["main_module"]["name"], "fixture_server.exe")
        self.assertEqual(report["main_module"]["selection"], "guess_labels")
        self.assertEqual(
            report["main_module"]["unique_rvas"],
            {
                "total": 3,
                "high_confidence_guess": 1,
                "low_confidence_guess": 1,
                "unresolved": 1,
                "conflicting": 0,
                "useful_guess": 2,
            },
        )

        main_self = report["coverage"]["main_module_self"]
        self.assertEqual(main_self["total_weight"], 140.0)
        self.assertEqual(main_self["high_confidence_guess_weight"], 50.0)
        self.assertEqual(main_self["low_confidence_guess_weight"], 30.0)
        self.assertEqual(main_self["resolved_symbol_weight"], 20.0)
        self.assertAlmostEqual(main_self["useful_guess_percent"], 80.0 / 140.0 * 100.0)
        self.assertEqual(main_self["uncovered_weight"], 40.0)

        all_self = report["coverage"]["all_threads_self"]
        self.assertEqual(all_self["total_weight"], 150.0)
        self.assertAlmostEqual(all_self["useful_guess_percent"], 80.0 / 150.0 * 100.0)
        self.assertEqual(all_self["uncovered_weight"], 50.0)

        main_depth = report["coverage"]["main_module_depth_weighted_total"]
        self.assertEqual(main_depth["total_weight"], 220.0)
        self.assertEqual(main_depth["high_confidence_guess_weight"], 100.0)
        self.assertEqual(main_depth["low_confidence_guess_weight"], 30.0)
        self.assertAlmostEqual(main_depth["useful_guess_percent"], 130.0 / 220.0 * 100.0)

        all_depth = report["coverage"]["all_threads_depth_weighted_total"]
        self.assertEqual(all_depth["total_weight"], 230.0)
        self.assertAlmostEqual(all_depth["useful_guess_percent"], 130.0 / 230.0 * 100.0)

        self.assertEqual(report["evidence_sources"]["vtable"]["unique_rvas"], 1)
        self.assertEqual(report["evidence_sources"]["vtable"]["self_weight"], 50.0)
        self.assertEqual(
            report["evidence_sources"]["vtable"]["high_confidence_depth_weighted_total"],
            100.0,
        )
        self.assertEqual(report["evidence_sources"]["str"]["low_confidence_unique_rvas"], 1)
        self.assertEqual(
            [item["rva"] for item in report["main_module"]["rvas"]],
            ["0x1000", "0x2000", "0x3000"],
        )
        self.assertEqual(report["main_module"]["rvas"][1]["evidence_source"], "vtable")
        self.assertEqual(report["main_module"]["rvas"][2]["confidence"], "low")
        self.assertEqual(report["unresolved_hotspots"][0]["rva"], "0x1000")
        self.assertEqual(report["unresolved_hotspots"][0]["self_weight"], 40.0)
        self.assertEqual(report["symbol_guess_build"]["function ranges"], 123)
        self.assertEqual(report["symbol_guess_build"]["batch microseconds"], 45)
        human = evaluator.render_human(report)
        self.assertIn("Main module: fixture_server.exe", human)
        self.assertIn("0x1000: self=40.000", human)
        self.assertEqual(
            report,
            evaluator.evaluate_profile(evaluator.parse_profile(synthetic_profile()), top=10),
        )

    def test_raw_and_gzip_loading_and_cli_json(self) -> None:
        payload = synthetic_profile()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw_path = root / "profile.pb"
            gzip_path = root / "profile.sparkprofile"
            raw_path.write_bytes(payload)
            gzip_path.write_bytes(gzip.compress(payload))

            raw, raw_kind, raw_size = evaluator.load_profile_bytes(str(raw_path), len(payload) + 1)
            packed, packed_kind, packed_size = evaluator.load_profile_bytes(
                str(gzip_path), len(payload) + 1
            )
            self.assertEqual(raw, payload)
            self.assertEqual(packed, payload)
            self.assertEqual(raw_kind, "raw")
            self.assertEqual(packed_kind, "gzip")
            self.assertEqual(raw_size, len(payload))
            self.assertEqual(packed_size, gzip_path.stat().st_size)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "profile_evaluator.py"),
                    str(gzip_path),
                    "--json",
                    "--top",
                    "1",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            output = json.loads(completed.stdout)
            self.assertEqual(output["input"]["compression"], "gzip")
            self.assertEqual(output["main_module"]["unique_rvas"]["total"], 3)
            self.assertEqual(len(output["unresolved_hotspots"]), 1)

    def test_explicit_module_and_allocation_unit(self) -> None:
        profile = evaluator.parse_profile(synthetic_profile())
        profile.sampler_mode = 1
        report = evaluator.evaluate_profile(profile, r"C:\server\fixture_server.exe")
        self.assertEqual(report["weight_unit"], "bytes")
        self.assertEqual(report["main_module"]["selection"], "explicit")
        with self.assertRaisesRegex(evaluator.ProfileError, "not present"):
            evaluator.evaluate_profile(profile, "missing.exe")

    def test_legacy_and_unknown_evidence_are_low_confidence(self) -> None:
        legacy_rva, legacy = evaluator.parse_method('0x10 ("old string label")')
        future_rva, future = evaluator.parse_method("0x20 (future: useful evidence)")
        high_rva, high = evaluator.parse_method("0x30 (thunk: Worker::run)")
        self.assertEqual(legacy_rva, 0x10)
        self.assertEqual(legacy.confidence, "low")
        self.assertEqual(legacy.source, "legacy")
        self.assertEqual(future_rva, 0x20)
        self.assertEqual(future.confidence, "low")
        self.assertEqual(high_rva, 0x30)
        self.assertEqual(high.confidence, "high")

    def test_conflicting_labels_for_one_rva_are_not_counted(self) -> None:
        nodes = [
            node("fixture_server.exe", "0x10 (vtable: A::vfn[0])", 2.0),
            node("fixture_server.exe", "0x10 (vtable: B::vfn[0])", 3.0),
        ]
        payload = message(1, field(15, 0, 0)) + message(
            2, thread("conflict", nodes, 5.0, [0, 1])
        )
        report = evaluator.evaluate_profile(evaluator.parse_profile(payload))
        counts = report["main_module"]["unique_rvas"]
        self.assertEqual(counts["conflicting"], 1)
        self.assertEqual(counts["useful_guess"], 0)
        self.assertEqual(report["coverage"]["main_module_self"]["useful_guess_weight"], 0.0)
        self.assertEqual(report["unresolved_hotspots"][0]["state"], "conflicting")

    def test_function_normalization_and_level_tick_subtree(self) -> None:
        nodes = [
            node(
                "fixture_server.exe",
                "0x1000 (vtable: Level::vfn[3])",
                20.0,
                sampled_rva=0x1015,
                function_rva=0x1000,
            ),
            node(
                "fixture_server.exe",
                "Level::tick()",
                20.0,
                [0],
                sampled_rva=0x5004,
            ),
        ]
        payload = message(1, field(15, 0, 0)) + message(
            2, thread("Server thread", nodes, 20.0, [1])
        )
        report = evaluator.evaluate_profile(
            evaluator.parse_profile(payload), "fixture_server.exe", top=20
        )

        self.assertTrue(report["normalization"]["extension_present"])
        self.assertEqual(report["normalization"]["unique_sampled_pcs"], 1)
        self.assertEqual(report["normalization"]["unique_function_roots"], 1)
        self.assertEqual(report["normalization"]["unique_pc_coverage_percent"], 100.0)
        self.assertEqual(report["main_module"]["rvas"][0]["sampled_pcs"], ["0x1015"])
        self.assertTrue(report["main_module"]["rvas"][0]["function_range_validated"])

        subtree = report["subtree"]
        self.assertTrue(subtree["found"])
        self.assertEqual(subtree["anchor_occurrences"], 1)
        self.assertEqual(subtree["normalization"]["unique_pc_coverage_percent"], 100.0)
        self.assertEqual(subtree["hotspot_explanation"]["top_20"]["available"], 1)
        self.assertEqual(subtree["hotspot_explanation"]["top_20"]["useful_percent"], 100.0)

    def test_malformed_wire_and_tree_are_rejected(self) -> None:
        with self.assertRaisesRegex(evaluator.ProfileError, "exceeds its message"):
            evaluator.parse_profile(b"\x12\x08short")

        bad_reference = message(1, field(15, 0, 0)) + message(
            2,
            thread(
                "bad",
                [node("fixture_server.exe", "0x10", 1.0, [4])],
                1.0,
                [0],
            ),
        )
        with self.assertRaisesRegex(evaluator.ProfileError, "out-of-range"):
            evaluator.evaluate_profile(evaluator.parse_profile(bad_reference))

        cycle = evaluator.ThreadTree(
            name="cycle",
            nodes=[
                evaluator.StackNode(times=[1.0], children=[1]),
                evaluator.StackNode(times=[1.0], children=[0]),
            ],
            times=[1.0],
            roots=[],
        )
        with self.assertRaisesRegex(evaluator.ProfileError, "cycle"):
            evaluator.validate_thread(cycle)

        excessive_children = message(1, field(15, 0, 0)) + message(
            2,
            thread(
                "weights",
                [
                    node("fixture_server.exe", "0x10", 2.0),
                    node("fixture_server.exe", "0x20", 1.0, [0]),
                ],
                1.0,
                [1],
            ),
        )
        with self.assertRaisesRegex(evaluator.ProfileError, "child totals exceed"):
            evaluator.evaluate_profile(evaluator.parse_profile(excessive_children))

    def test_decompression_limit_is_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "large.sparkprofile"
            path.write_bytes(gzip.compress(b"x" * 1024))
            with self.assertRaisesRegex(evaluator.ProfileError, "decompressed profile exceeds"):
                evaluator.load_profile_bytes(str(path), 100)


if __name__ == "__main__":
    unittest.main()
