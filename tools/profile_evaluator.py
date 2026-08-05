#!/usr/bin/env python3
"""Evaluate native symbol guesses in a spark SamplerData profile.

The reader intentionally implements only the protobuf wire format and the
SamplerData fields needed for evaluation. It does not require protoc or the
protobuf Python package. Both raw protobuf and gzip-compressed sparkprofile
files are accepted.
"""

from __future__ import annotations

import argparse
import gzip
import io
import json
import math
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator


DEFAULT_MAX_BYTES = 512 * 1024 * 1024
KNOWN_EVIDENCE = {"rtti", "str", "vtable", "thunk", "type"}
ADDRESS_METHOD_RE = re.compile(r"^0x([0-9a-fA-F]+)(?: \((.*)\))?$")
EVIDENCE_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_-]*)(\?)?:\s*(.+)$")


class ProfileError(ValueError):
    pass


@dataclass(frozen=True)
class Field:
    number: int
    wire_type: int
    value: int | memoryview


@dataclass
class StackNode:
    class_name: str = ""
    method_name: str = ""
    times: list[float] = field(default_factory=list)
    children: list[int] = field(default_factory=list)
    sampled_rva: int | None = None
    function_rva: int | None = None

    @property
    def total(self) -> float:
        return math.fsum(self.times)


@dataclass
class ThreadTree:
    name: str = ""
    nodes: list[StackNode] = field(default_factory=list)
    times: list[float] = field(default_factory=list)
    roots: list[int] = field(default_factory=list)


@dataclass
class Profile:
    threads: list[ThreadTree] = field(default_factory=list)
    sampler_mode: int = 0
    metadata: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class Guess:
    label: str
    source: str
    confidence: str


@dataclass
class FrameRecord:
    module: str
    method: str
    self_weight: float
    total_weight: float
    depth: int
    rva: int | None
    guess: Guess | None
    sampled_rva: int | None
    function_rva: int | None
    in_subtree: bool


@dataclass
class RvaState:
    state: str
    guess: Guess | None
    labels: list[str]


def read_varint(data: memoryview, offset: int, end: int) -> tuple[int, int]:
    value = 0
    shift = 0
    for _ in range(10):
        if offset >= end:
            raise ProfileError("truncated protobuf varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
    raise ProfileError("protobuf varint exceeds 64 bits")


def iter_fields(data: memoryview) -> Iterator[Field]:
    offset = 0
    end = len(data)
    while offset < end:
        tag, offset = read_varint(data, offset, end)
        number = tag >> 3
        wire_type = tag & 7
        if number == 0:
            raise ProfileError("protobuf field number zero is invalid")
        if wire_type == 0:
            value, offset = read_varint(data, offset, end)
            yield Field(number, wire_type, value)
        elif wire_type == 1:
            if end - offset < 8:
                raise ProfileError("truncated protobuf fixed64")
            value = int.from_bytes(data[offset : offset + 8], "little")
            offset += 8
            yield Field(number, wire_type, value)
        elif wire_type == 2:
            size, offset = read_varint(data, offset, end)
            if size > end - offset:
                raise ProfileError("protobuf length-delimited field exceeds its message")
            value = data[offset : offset + size]
            offset += size
            yield Field(number, wire_type, value)
        elif wire_type == 5:
            if end - offset < 4:
                raise ProfileError("truncated protobuf fixed32")
            value = int.from_bytes(data[offset : offset + 4], "little")
            offset += 4
            yield Field(number, wire_type, value)
        else:
            raise ProfileError(f"unsupported protobuf wire type {wire_type}")


def as_bytes(field: Field, context: str) -> memoryview:
    if field.wire_type != 2 or not isinstance(field.value, memoryview):
        raise ProfileError(f"{context} has the wrong protobuf wire type")
    return field.value


def decode_text(data: memoryview, context: str) -> str:
    try:
        return data.tobytes().decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProfileError(f"{context} is not valid UTF-8") from exc


def parse_packed_doubles(field: Field, context: str) -> list[float]:
    if field.wire_type == 1 and isinstance(field.value, int):
        values = [struct.unpack("<d", field.value.to_bytes(8, "little"))[0]]
    else:
        payload = as_bytes(field, context)
        if len(payload) % 8:
            raise ProfileError(f"{context} packed double payload is misaligned")
        values = [item[0] for item in struct.iter_unpack("<d", payload)]
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ProfileError(f"{context} contains a negative or non-finite weight")
    return values


def parse_packed_int32(field: Field, context: str) -> list[int]:
    if field.wire_type == 0 and isinstance(field.value, int):
        return [field.value & 0xFFFFFFFF]
    payload = as_bytes(field, context)
    values: list[int] = []
    offset = 0
    while offset < len(payload):
        value, offset = read_varint(payload, offset, len(payload))
        values.append(value & 0xFFFFFFFF)
    return values


def parse_node(data: memoryview) -> StackNode:
    node = StackNode()
    for item in iter_fields(data):
        if item.number == 3:
            node.class_name = decode_text(as_bytes(item, "StackTraceNode.class_name"), "class_name")
        elif item.number == 4:
            node.method_name = decode_text(as_bytes(item, "StackTraceNode.method_name"), "method_name")
        elif item.number == 8:
            node.times.extend(parse_packed_doubles(item, "StackTraceNode.times"))
        elif item.number == 9:
            node.children.extend(parse_packed_int32(item, "StackTraceNode.children_refs"))
        elif item.number == 1001:
            if item.wire_type != 0 or not isinstance(item.value, int):
                raise ProfileError("StackTraceNode.sampled_rva has the wrong wire type")
            node.sampled_rva = item.value
        elif item.number == 1002:
            if item.wire_type != 0 or not isinstance(item.value, int):
                raise ProfileError("StackTraceNode.function_rva has the wrong wire type")
            node.function_rva = item.value
    return node


def parse_thread(data: memoryview) -> ThreadTree:
    thread = ThreadTree()
    for item in iter_fields(data):
        if item.number == 1:
            thread.name = decode_text(as_bytes(item, "ThreadNode.name"), "thread name")
        elif item.number == 3:
            thread.nodes.append(parse_node(as_bytes(item, "ThreadNode.children")))
        elif item.number == 4:
            thread.times.extend(parse_packed_doubles(item, "ThreadNode.times"))
        elif item.number == 5:
            thread.roots.extend(parse_packed_int32(item, "ThreadNode.children_refs"))
    return thread


def parse_string_map_entry(data: memoryview) -> tuple[str, str]:
    key = ""
    value = ""
    for item in iter_fields(data):
        if item.number == 1:
            key = decode_text(as_bytes(item, "map key"), "map key")
        elif item.number == 2:
            value = decode_text(as_bytes(item, "map value"), "map value")
    return key, value


def parse_metadata(data: memoryview, profile: Profile) -> None:
    for item in iter_fields(data):
        if item.number == 14:
            key, value = parse_string_map_entry(as_bytes(item, "extra_platform_metadata"))
            if key:
                profile.metadata[key] = value
        elif item.number == 15:
            if item.wire_type != 0 or not isinstance(item.value, int):
                raise ProfileError("SamplerMetadata.sampler_mode has the wrong wire type")
            profile.sampler_mode = item.value


def parse_profile(data: bytes) -> Profile:
    profile = Profile()
    view = memoryview(data)
    for item in iter_fields(view):
        if item.number == 1:
            parse_metadata(as_bytes(item, "SamplerData.metadata"), profile)
        elif item.number == 2:
            profile.threads.append(parse_thread(as_bytes(item, "SamplerData.threads")))
    if not profile.threads:
        raise ProfileError("SamplerData contains no threads")
    return profile


def load_profile_bytes(path: str, maximum: int = DEFAULT_MAX_BYTES) -> tuple[bytes, str, int]:
    if maximum <= 0:
        raise ProfileError("maximum input size must be positive")
    if path == "-":
        compressed = sys.stdin.buffer.read(maximum + 1)
    else:
        with open(path, "rb") as stream:
            compressed = stream.read(maximum + 1)
    if len(compressed) > maximum:
        raise ProfileError(f"input exceeds the {maximum}-byte safety limit")
    compressed_size = len(compressed)
    if compressed.startswith(b"\x1f\x8b"):
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
                data = stream.read(maximum + 1)
        except (EOFError, OSError) as exc:
            raise ProfileError(f"invalid gzip profile: {exc}") from exc
        if len(data) > maximum:
            raise ProfileError(f"decompressed profile exceeds the {maximum}-byte safety limit")
        return data, "gzip", compressed_size
    return compressed, "raw", compressed_size


def parse_method(method: str) -> tuple[int | None, Guess | None]:
    match = ADDRESS_METHOD_RE.fullmatch(method)
    if not match:
        return None, None
    rva = int(match.group(1), 16)
    label = match.group(2)
    if label is None or not label.strip():
        return rva, None
    label = label.strip()
    evidence = EVIDENCE_RE.fullmatch(label)
    if evidence:
        source = evidence.group(1).lower()
        # Unknown source names remain useful but conservative. This also makes
        # the evaluator compatible with future evidence tags.
        confidence = "low" if evidence.group(2) or source not in KNOWN_EVIDENCE else "high"
        return rva, Guess(label, source, confidence)
    # Profiles produced before evidence tags used a bare quoted string or class
    # label. Count those as low confidence instead of silently treating them as
    # a current high-confidence guess.
    return rva, Guess(label, "legacy", "low")


def validate_thread(thread: ThreadTree) -> tuple[list[tuple[int, int]], float, int]:
    count = len(thread.nodes)
    parent_counts = [0] * count
    for parent, node in enumerate(thread.nodes):
        if len(set(node.children)) != len(node.children):
            raise ProfileError(f"thread {thread.name!r} node {parent} repeats a child reference")
        for child in node.children:
            if child >= count:
                raise ProfileError(f"thread {thread.name!r} has out-of-range child reference {child}")
            parent_counts[child] += 1
    if len(set(thread.roots)) != len(thread.roots):
        raise ProfileError(f"thread {thread.name!r} repeats a root reference")
    for root in thread.roots:
        if root >= count:
            raise ProfileError(f"thread {thread.name!r} has out-of-range root reference {root}")
        parent_counts[root] += 1
    if any(parents > 1 for parents in parent_counts):
        raise ProfileError(f"thread {thread.name!r} is a graph, not a call tree")

    colors = [0] * count
    for start in range(count):
        if colors[start] != 0:
            continue
        stack = [(start, False)]
        while stack:
            current, exiting = stack.pop()
            if exiting:
                colors[current] = 2
                continue
            if colors[current] == 1:
                raise ProfileError(f"thread {thread.name!r} contains a child-reference cycle")
            if colors[current] == 2:
                continue
            colors[current] = 1
            stack.append((current, True))
            for child in thread.nodes[current].children:
                if colors[child] == 1:
                    raise ProfileError(f"thread {thread.name!r} contains a child-reference cycle")
                if colors[child] == 0:
                    stack.append((child, False))

    reachable: list[tuple[int, int]] = []
    stack = [(root, 1) for root in reversed(thread.roots)]
    while stack:
        index, depth = stack.pop()
        reachable.append((index, depth))
        for child in reversed(thread.nodes[index].children):
            stack.append((child, depth + 1))

    root_total = math.fsum(thread.nodes[index].total for index in thread.roots)
    thread_total = math.fsum(thread.times) if thread.times else root_total
    tolerance = max(1e-9, abs(thread_total) * 1e-9)
    if root_total > thread_total + tolerance:
        raise ProfileError(f"thread {thread.name!r} child totals exceed the thread total")
    return reachable, thread_total, count - len(reachable)


def collect_records(
    profile: Profile, subtree_method: str | None = None
) -> tuple[list[FrameRecord], float, int]:
    records: list[FrameRecord] = []
    all_thread_self = 0.0
    unreachable = 0
    for thread in profile.threads:
        _, thread_total, missing = validate_thread(thread)
        unreachable += missing
        all_thread_self += thread_total
        stack = [(root, 1, False) for root in reversed(thread.roots)]
        while stack:
            index, depth, parent_in_subtree = stack.pop()
            node = thread.nodes[index]
            in_subtree = parent_in_subtree or (
                subtree_method is not None and node.method_name == subtree_method
            )
            child_total = math.fsum(thread.nodes[child].total for child in node.children)
            tolerance = max(1e-9, abs(node.total) * 1e-9)
            if child_total > node.total + tolerance:
                raise ProfileError(
                    f"thread {thread.name!r} node {index} child totals exceed its total"
                )
            self_weight = max(0.0, node.total - child_total)
            rva, guess = parse_method(node.method_name)
            if node.function_rva is not None and rva != node.function_rva:
                raise ProfileError(
                    f"thread {thread.name!r} function_rva does not match its displayed RVA"
                )
            records.append(
                FrameRecord(
                    node.class_name,
                    node.method_name,
                    self_weight,
                    node.total,
                    depth,
                    rva,
                    guess,
                    node.sampled_rva if node.sampled_rva is not None else rva,
                    node.function_rva,
                    in_subtree,
                )
            )
            for child in reversed(node.children):
                stack.append((child, depth + 1, in_subtree))
    return records, all_thread_self, unreachable


def module_basename(value: str) -> str:
    return value.replace("\\", "/").rsplit("/", 1)[-1]


def choose_main_module(records: list[FrameRecord], requested: str | None) -> tuple[str, str]:
    modules = {record.module for record in records if record.module}
    if requested:
        wanted = module_basename(requested).casefold()
        matches = sorted(module for module in modules if module_basename(module).casefold() == wanted)
        if not matches:
            raise ProfileError(f"requested main module {requested!r} is not present in the profile")
        if len(matches) > 1:
            raise ProfileError(f"requested main module {requested!r} matches multiple profile modules")
        return matches[0], "explicit"

    guessed: dict[str, tuple[set[int], float]] = {}
    for record in records:
        if record.rva is None or record.guess is None or not record.module:
            continue
        rvas, weight = guessed.get(record.module, (set(), 0.0))
        rvas.add(record.rva)
        guessed[record.module] = (rvas, weight + record.self_weight)
    if guessed:
        ranked = sorted(guessed, key=lambda module: (-len(guessed[module][0]), -guessed[module][1], module))
        return ranked[0], "guess_labels"

    raw: dict[str, tuple[set[int], float]] = {}
    for record in records:
        if record.rva is None or not record.module:
            continue
        rvas, weight = raw.get(record.module, (set(), 0.0))
        rvas.add(record.rva)
        raw[record.module] = (rvas, weight + record.self_weight)
    if raw:
        ranked = sorted(raw, key=lambda module: (-raw[module][1], -len(raw[module][0]), module))
        return ranked[0], "weighted_rva_fallback"
    raise ProfileError("cannot infer the main module; pass --main-module")


def rva_states(records: Iterable[FrameRecord], main_module: str) -> dict[int, RvaState]:
    labels: dict[int, dict[str, Guess]] = {}
    all_rvas: set[int] = set()
    for record in records:
        if record.module != main_module or record.rva is None:
            continue
        all_rvas.add(record.rva)
        if record.guess is not None:
            labels.setdefault(record.rva, {})[record.guess.label] = record.guess
    result: dict[int, RvaState] = {}
    for rva in all_rvas:
        guesses = labels.get(rva, {})
        if not guesses:
            result[rva] = RvaState("unresolved", None, [])
        elif len(guesses) != 1:
            result[rva] = RvaState("conflicting", None, sorted(guesses))
        else:
            guess = next(iter(guesses.values()))
            result[rva] = RvaState(f"{guess.confidence}_guess", guess, [guess.label])
    return result


def percentage(numerator: float, denominator: float) -> float | None:
    return numerator * 100.0 / denominator if denominator > 0 else None


def coverage(total: float, weights: dict[str, float]) -> dict[str, float | None]:
    high = weights.get("high_guess", 0.0)
    low = weights.get("low_guess", 0.0)
    resolved = weights.get("resolved_symbol", 0.0)
    useful = high + low
    return {
        "total_weight": total,
        "high_confidence_guess_weight": high,
        "high_confidence_readable_weight": high + resolved,
        "low_confidence_guess_weight": low,
        "useful_guess_weight": useful,
        "resolved_symbol_weight": resolved,
        # In all-thread metrics this also includes frames outside the main
        # module, so "uncovered" is more precise than "unresolved".
        "uncovered_weight": max(0.0, total - useful - resolved),
        "high_confidence_guess_percent": percentage(high, total),
        "high_confidence_readable_percent": percentage(high + resolved, total),
        "useful_guess_percent": percentage(useful, total),
        "readable_percent": percentage(useful + resolved, total),
    }


def record_state(record: FrameRecord, states: dict[int, RvaState]) -> str:
    if record.rva is None:
        return "resolved_symbol"
    return states[record.rva].state


def normalization_metrics(
    records: Iterable[FrameRecord], main_module: str
) -> dict[str, object]:
    selected = [
        record
        for record in records
        if record.module == main_module
        and record.rva is not None
        and record.sampled_rva is not None
    ]
    sampled_pcs = {record.sampled_rva for record in selected}
    covered_pcs = {
        record.sampled_rva
        for record in selected
        if record.function_rva is not None
    }
    function_roots = {
        record.function_rva
        for record in selected
        if record.function_rva is not None
    }
    self_total = math.fsum(record.self_weight for record in selected)
    self_covered = math.fsum(
        record.self_weight for record in selected if record.function_rva is not None
    )
    depth_total = math.fsum(record.total_weight for record in selected)
    depth_covered = math.fsum(
        record.total_weight for record in selected if record.function_rva is not None
    )
    return {
        "extension_present": any(record.function_rva is not None for record in selected),
        "unique_sampled_pcs": len(sampled_pcs),
        "unique_validated_sampled_pcs": len(covered_pcs),
        "unique_function_roots": len(function_roots),
        "unique_pc_coverage_percent": percentage(len(covered_pcs), len(sampled_pcs)),
        "self_coverage_percent": percentage(self_covered, self_total),
        "depth_weighted_total_coverage_percent": percentage(depth_covered, depth_total),
        "self_weight": self_total,
        "validated_self_weight": self_covered,
        "depth_weighted_total": depth_total,
        "validated_depth_weighted_total": depth_covered,
    }


def subtree_metrics(
    records: list[FrameRecord], states: dict[int, RvaState], main_module: str,
    method: str, top: int
) -> dict[str, object]:
    selected = [record for record in records if record.in_subtree]
    anchors = [record for record in selected if record.method == method]
    main_records = [record for record in selected if record.module == main_module]
    all_self_weights: dict[str, float] = {}
    main_self_weights: dict[str, float] = {}
    all_self = 0.0
    main_self = 0.0
    main_depth = 0.0
    for record in selected:
        state = record_state(record, states) if record.module == main_module else "resolved_symbol"
        all_self += record.self_weight
        all_self_weights[state] = all_self_weights.get(state, 0.0) + record.self_weight
        if record.module == main_module:
            main_self += record.self_weight
            main_depth += record.total_weight
            main_self_weights[state] = main_self_weights.get(state, 0.0) + record.self_weight

    functions: dict[tuple[str, object], dict[str, object]] = {}
    for record in main_records:
        if record.rva is None:
            key = ("symbol", record.method)
            label = record.method
        else:
            key = ("rva", record.rva)
            label = f"0x{record.rva:x}"
            if record.guess is not None:
                label += f" ({record.guess.label})"
        item = functions.setdefault(
            key,
            {
                "label": label,
                "state": record_state(record, states),
                "self_weight": 0.0,
                "depth_weighted_total": 0.0,
                "sampled_pcs": set(),
                "occurrences": 0,
            },
        )
        item["self_weight"] += record.self_weight
        item["depth_weighted_total"] += record.total_weight
        if record.sampled_rva is not None:
            item["sampled_pcs"].add(record.sampled_rva)
        item["occurrences"] += 1

    hotspots = [item for item in functions.values() if item["self_weight"] > 0]
    hotspots.sort(
        key=lambda item: (
            -item["self_weight"], -item["depth_weighted_total"], item["label"]
        )
    )
    for item in hotspots:
        item["sampled_pcs"] = [f"0x{rva:x}" for rva in sorted(item["sampled_pcs"])]

    explanation: dict[str, object] = {}
    for limit in (20, 50, 100):
        ranked = hotspots[:limit]
        count = len(ranked)
        high = sum(item["state"] in {"resolved_symbol", "high_guess"} for item in ranked)
        useful = sum(
            item["state"] in {"resolved_symbol", "high_guess", "low_guess"}
            for item in ranked
        )
        weight = math.fsum(item["self_weight"] for item in ranked)
        useful_weight = math.fsum(
            item["self_weight"]
            for item in ranked
            if item["state"] in {"resolved_symbol", "high_guess", "low_guess"}
        )
        explanation[f"top_{limit}"] = {
            "available": count,
            "high_confidence": high,
            "useful": useful,
            "high_confidence_percent": percentage(high, count),
            "useful_percent": percentage(useful, count),
            "self_weight": weight,
            "useful_self_weight": useful_weight,
            "useful_self_percent": percentage(useful_weight, weight),
        }

    return {
        "method": method,
        "found": bool(anchors),
        "anchor_occurrences": len(anchors),
        "main_module_self": coverage(main_self, main_self_weights),
        "all_modules_self": coverage(all_self, all_self_weights),
        "main_module_depth_weighted_total": coverage(main_depth, {
            state: math.fsum(
                record.total_weight
                for record in main_records
                if record_state(record, states) == state
            )
            for state in {record_state(record, states) for record in main_records}
        }),
        "normalization": normalization_metrics(main_records, main_module),
        "hotspot_explanation": explanation,
        "hotspots": hotspots[:top],
    }


def decode_metadata_value(value: str) -> object:
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value


def evaluate_profile(
    profile: Profile,
    main_module: str | None = None,
    top: int = 20,
    subtree_method: str | None = "Level::tick()",
) -> dict[str, object]:
    if top < 0:
        raise ProfileError("hotspot count cannot be negative")
    records, all_thread_self, unreachable_nodes = collect_records(profile, subtree_method)
    selected_module, selection = choose_main_module(records, main_module)
    states = rva_states(records, selected_module)

    main_self = 0.0
    main_depth_total = 0.0
    all_depth_total = math.fsum(record.total_weight for record in records)
    self_weights: dict[str, float] = {}
    depth_weights: dict[str, float] = {}
    all_self_weights: dict[str, float] = {}
    all_depth_weights: dict[str, float] = {}
    source_data: dict[str, dict[str, object]] = {}
    hotspot_data: dict[int, dict[str, float | int]] = {}
    rva_data: dict[int, dict[str, object]] = {}

    for record in records:
        if record.module != selected_module:
            continue
        main_self += record.self_weight
        main_depth_total += record.total_weight
        if record.rva is None:
            state_name = "resolved_symbol"
            guess = None
        else:
            state = states[record.rva]
            state_name = state.state
            guess = state.guess
        self_weights[state_name] = self_weights.get(state_name, 0.0) + record.self_weight
        depth_weights[state_name] = depth_weights.get(state_name, 0.0) + record.total_weight
        all_self_weights[state_name] = all_self_weights.get(state_name, 0.0) + record.self_weight
        all_depth_weights[state_name] = all_depth_weights.get(state_name, 0.0) + record.total_weight

        if record.rva is not None:
            rva_item = rva_data.setdefault(
                record.rva,
                {
                    "self_weight": 0.0,
                    "depth_weighted_total": 0.0,
                    "occurrences": 0,
                    "sampled_pcs": set(),
                    "function_range_validated": False,
                },
            )
            rva_item["self_weight"] += record.self_weight
            rva_item["depth_weighted_total"] += record.total_weight
            rva_item["occurrences"] += 1
            if record.sampled_rva is not None:
                rva_item["sampled_pcs"].add(record.sampled_rva)
            rva_item["function_range_validated"] = (
                rva_item["function_range_validated"] or record.function_rva is not None
            )

        if guess is not None:
            source = source_data.setdefault(
                guess.source,
                {
                    "unique_rvas": set(),
                    "high_confidence_unique_rvas": set(),
                    "low_confidence_unique_rvas": set(),
                    "self_weight": 0.0,
                    "depth_weighted_total": 0.0,
                    "high_confidence_self_weight": 0.0,
                    "low_confidence_self_weight": 0.0,
                    "high_confidence_depth_weighted_total": 0.0,
                    "low_confidence_depth_weighted_total": 0.0,
                },
            )
            assert record.rva is not None
            source["unique_rvas"].add(record.rva)
            source[f"{guess.confidence}_confidence_unique_rvas"].add(record.rva)
            source["self_weight"] += record.self_weight
            source["depth_weighted_total"] += record.total_weight
            source[f"{guess.confidence}_confidence_self_weight"] += record.self_weight
            source[f"{guess.confidence}_confidence_depth_weighted_total"] += record.total_weight
        elif record.rva is not None and state_name in {"unresolved", "conflicting"}:
            hotspot = hotspot_data.setdefault(
                record.rva,
                {"self_weight": 0.0, "depth_weighted_total": 0.0, "occurrences": 0},
            )
            hotspot["self_weight"] += record.self_weight
            hotspot["depth_weighted_total"] += record.total_weight
            hotspot["occurrences"] += 1

    unique_counts = {
        "total": len(states),
        "high_confidence_guess": sum(state.state == "high_guess" for state in states.values()),
        "low_confidence_guess": sum(state.state == "low_guess" for state in states.values()),
        "unresolved": sum(state.state == "unresolved" for state in states.values()),
        "conflicting": sum(state.state == "conflicting" for state in states.values()),
    }
    unique_counts["useful_guess"] = (
        unique_counts["high_confidence_guess"] + unique_counts["low_confidence_guess"]
    )

    evidence: dict[str, object] = {}
    for name in sorted(source_data):
        item = source_data[name]
        evidence[name] = {
            "unique_rvas": len(item["unique_rvas"]),
            "high_confidence_unique_rvas": len(item["high_confidence_unique_rvas"]),
            "low_confidence_unique_rvas": len(item["low_confidence_unique_rvas"]),
            "self_weight": item["self_weight"],
            "depth_weighted_total": item["depth_weighted_total"],
            "high_confidence_self_weight": item["high_confidence_self_weight"],
            "low_confidence_self_weight": item["low_confidence_self_weight"],
            "high_confidence_depth_weighted_total": item[
                "high_confidence_depth_weighted_total"
            ],
            "low_confidence_depth_weighted_total": item[
                "low_confidence_depth_weighted_total"
            ],
        }

    rvas = []
    for rva in sorted(states):
        state = states[rva]
        item = rva_data[rva]
        rvas.append(
            {
                "rva": f"0x{rva:x}",
                "state": state.state,
                "label": state.guess.label if state.guess is not None else None,
                "evidence_source": state.guess.source if state.guess is not None else None,
                "confidence": state.guess.confidence if state.guess is not None else None,
                "conflicting_labels": state.labels if state.state == "conflicting" else [],
                "self_weight": item["self_weight"],
                "depth_weighted_total": item["depth_weighted_total"],
                "occurrences": item["occurrences"],
                "sampled_pcs": [f"0x{pc:x}" for pc in sorted(item["sampled_pcs"])],
                "function_range_validated": item["function_range_validated"],
            }
        )

    hotspots = []
    for rva, item in hotspot_data.items():
        state = states[rva]
        hotspots.append(
            {
                "rva": f"0x{rva:x}",
                "state": state.state,
                "conflicting_labels": state.labels if state.state == "conflicting" else [],
                "self_weight": item["self_weight"],
                "self_percent_of_main_module": percentage(item["self_weight"], main_self),
                "self_percent_of_all_threads": percentage(item["self_weight"], all_thread_self),
                "depth_weighted_total": item["depth_weighted_total"],
                "occurrences": item["occurrences"],
            }
        )
    hotspots.sort(key=lambda item: (-item["self_weight"], -item["depth_weighted_total"], item["rva"]))

    symbol_metadata = {
        key.removeprefix("Symbol guess "): decode_metadata_value(value)
        for key, value in sorted(profile.metadata.items())
        if key.startswith("Symbol guess ")
    }
    return {
        "schema_version": 1,
        "weight_unit": "bytes" if profile.sampler_mode == 1 else "milliseconds",
        "threads": len(profile.threads),
        "main_module": {
            "name": selected_module,
            "selection": selection,
            "unique_rvas": unique_counts,
            "rvas": rvas,
        },
        "coverage": {
            "main_module_self": coverage(main_self, self_weights),
            "all_threads_self": coverage(all_thread_self, all_self_weights),
            "main_module_depth_weighted_total": coverage(main_depth_total, depth_weights),
            "all_threads_depth_weighted_total": coverage(all_depth_total, all_depth_weights),
        },
        "evidence_sources": evidence,
        "normalization": normalization_metrics(records, selected_module),
        "subtree": (
            subtree_metrics(records, states, selected_module, subtree_method, top)
            if subtree_method is not None
            else None
        ),
        "unresolved_hotspots": hotspots[:top],
        "symbol_guess_build": symbol_metadata,
        "diagnostics": {
            "parsed_stack_nodes": len(records),
            "unreachable_stack_nodes": unreachable_nodes,
            "conflicting_rvas": unique_counts["conflicting"],
        },
        "metric_definitions": {
            "self": "node inclusive weight minus the inclusive weights of its direct children",
            "all_threads_self": "all thread-root weight, including frames outside the main module",
            "all_thread_coverage_scope": (
                "main-module guesses and resolved main-module symbols divided by all-thread weight"
            ),
            "depth_weighted_total": (
                "inclusive node weight summed once per stack node, so deeper samples contribute once per depth"
            ),
        },
    }


def format_percent(value: object) -> str:
    return "n/a" if value is None else f"{float(value):.2f}%"


def render_human(report: dict[str, object]) -> str:
    module = report["main_module"]
    counts = module["unique_rvas"]
    lines = [
        f"Profile: {report['threads']} thread(s), weight unit: {report['weight_unit']}",
        f"Main module: {module['name']} (selection: {module['selection']})",
        (
            "Unique main-module RVAs: "
            f"{counts['total']} total, {counts['high_confidence_guess']} high, "
            f"{counts['low_confidence_guess']} low, {counts['unresolved']} unresolved, "
            f"{counts['conflicting']} conflicting"
        ),
        "Coverage:",
    ]
    labels = {
        "main_module_self": "  main-module Self",
        "all_threads_self": "  all-thread Self",
        "main_module_depth_weighted_total": "  main-module depth-weighted Total",
        "all_threads_depth_weighted_total": "  all-thread depth-weighted Total",
    }
    for key, label in labels.items():
        item = report["coverage"][key]
        lines.append(
            f"{label}: useful={format_percent(item['useful_guess_percent'])}, "
            f"high={format_percent(item['high_confidence_guess_percent'])}, "
            f"readable={format_percent(item['readable_percent'])} "
            f"(total={item['total_weight']:.3f})"
        )
    normalization = report["normalization"]
    lines.append(
        "Function normalization: "
        f"sampled-pcs={normalization['unique_sampled_pcs']}, "
        f"validated-pcs={normalization['unique_validated_sampled_pcs']}, "
        f"roots={normalization['unique_function_roots']}, "
        f"pc-coverage={format_percent(normalization['unique_pc_coverage_percent'])}, "
        f"self-coverage={format_percent(normalization['self_coverage_percent'])}"
    )
    subtree = report.get("subtree")
    if subtree is not None:
        lines.append(
            f"Subtree {subtree['method']}: "
            + (f"found ({subtree['anchor_occurrences']} anchor(s))" if subtree["found"] else "not found")
        )
        if subtree["found"]:
            item = subtree["main_module_self"]
            lines.append(
                "  main-module Self: "
                f"high-readable={format_percent(item['high_confidence_readable_percent'])}, "
                f"useful-readable={format_percent(item['readable_percent'])}, "
                f"total={item['total_weight']:.3f}"
            )
            item = subtree["main_module_depth_weighted_total"]
            lines.append(
                "  main-module depth-weighted Total: "
                f"useful-readable={format_percent(item['readable_percent'])}, "
                f"total={item['total_weight']:.3f}"
            )
            normalized = subtree["normalization"]
            lines.append(
                "  function normalization: "
                f"pc-coverage={format_percent(normalized['unique_pc_coverage_percent'])}, "
                f"self-coverage={format_percent(normalized['self_coverage_percent'])}"
            )
            explanations = subtree["hotspot_explanation"]
            lines.append(
                "  hotspot explanation: "
                + ", ".join(
                    f"{name.replace('_', ' ')}={format_percent(value['useful_percent'])}"
                    for name, value in explanations.items()
                )
            )
    lines.append("Evidence sources:")
    if not report["evidence_sources"]:
        lines.append("  (none)")
    for source, item in report["evidence_sources"].items():
        lines.append(
            f"  {source}: rvas={item['unique_rvas']}, self={item['self_weight']:.3f}, "
            f"depth-total={item['depth_weighted_total']:.3f}"
        )
    lines.append("Highest unresolved main-module Self hotspots:")
    if not report["unresolved_hotspots"]:
        lines.append("  (none)")
    for item in report["unresolved_hotspots"]:
        lines.append(
            f"  {item['rva']}: self={item['self_weight']:.3f} "
            f"({format_percent(item['self_percent_of_main_module'])} of main), "
            f"depth-total={item['depth_weighted_total']:.3f}, state={item['state']}"
        )
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Evaluate runtime symbol-guess coverage in a spark SamplerData profile"
    )
    parser.add_argument("profile", help="raw protobuf or gzip sparkprofile path; '-' reads stdin")
    parser.add_argument(
        "--main-module",
        help="main executable basename; auto-detected from guessed RVA frames when omitted",
    )
    parser.add_argument("--top", type=int, default=20, help="number of unresolved Self hotspots")
    parser.add_argument(
        "--subtree",
        default="Level::tick()",
        help="exact method name whose descendant subtree is evaluated; empty disables it",
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument(
        "--max-bytes",
        type=int,
        default=DEFAULT_MAX_BYTES,
        help="maximum compressed and decompressed input size",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        data, compression, compressed_size = load_profile_bytes(args.profile, args.max_bytes)
        profile = parse_profile(data)
        report = evaluate_profile(profile, args.main_module, args.top, args.subtree or None)
        report["input"] = {
            "path": args.profile,
            "compression": compression,
            "compressed_bytes": compressed_size,
            "protobuf_bytes": len(data),
        }
    except (OSError, ProfileError) as exc:
        print(f"profile evaluator: {exc}", file=sys.stderr)
        return 2
    if args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print(render_human(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
