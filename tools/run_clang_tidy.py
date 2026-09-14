#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
from collections.abc import Iterable


SHARDS = ("core", "native", "application", "tests-core", "selftest")

TARGET_SHARDS = {
    "spark_profiling_time": "core",
    "spark_core": "core",
    "spark_native": "native",
    "spark_application": "application",
    "spark_papi_integration": "application",
    "spark": "application",
}

TARGET_PATTERN = re.compile(r"(?:^|[/\\])CMakeFiles[/\\]([^/\\]+)\.dir(?:[/\\]|$)")
C_CPP_EXTENSIONS = frozenset({".c", ".cc", ".cpp", ".cxx", ".c++"})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run clang-tidy on project-owned translation units")
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--jobs", type=int, default=min(4, max(1, os.cpu_count() or 1)))
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--match")
    parser.add_argument("--shard", choices=SHARDS)
    parser.add_argument("--list", action="store_true", help="List selected sources without running clang-tidy")
    return parser.parse_args()


def extract_target(output: str | None) -> str | None:
    """Extract the CMake target from a compile_commands output path."""
    if not output:
        return None
    match = TARGET_PATTERN.search(output)
    return match.group(1) if match else None


def _relative_source(source: str, root: pathlib.Path) -> tuple[pathlib.Path, pathlib.PurePosixPath] | None:
    path = pathlib.Path(source).resolve()
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    return path, pathlib.PurePosixPath(relative.as_posix())


def _is_c_cpp_translation_unit(source: pathlib.Path) -> bool:
    return source.suffix.lower() in C_CPP_EXTENSIONS


def _path_shard(relative: pathlib.PurePosixPath) -> str | None:
    parts = relative.parts
    if not parts:
        return None
    if parts[0] == "src":
        if len(parts) > 1 and parts[1] == "native":
            return "native"
        if len(parts) > 1 and parts[1] in {"application", "platform"}:
            return "application"
        if len(parts) > 1 and parts[1] in {"core", "net", "proto"}:
            return "core"
        if len(parts) == 2 and parts[1] == "plugin.cpp":
            return "application"
    elif parts[0] == "tests":
        if len(parts) > 1 and parts[1] == "native":
            return "native"
        if len(parts) > 1 and parts[1] in {"application", "platform"}:
            return "application"
        if len(parts) > 1 and parts[1] in {"core", "proto", "net"}:
            return "tests-core"
        return "selftest"
    return None


def classify_source(relative: pathlib.PurePosixPath, targets: Iterable[str | None]) -> str:
    """Assign one stable shard to a project source.

    Source paths own the assignment for known project areas. Target output is
    the deterministic fallback for any new source path under src/.
    """
    path_shard = _path_shard(relative)
    if path_shard is not None:
        return path_shard

    target_shards = {TARGET_SHARDS[target] for target in targets if target in TARGET_SHARDS}
    if len(target_shards) == 1:
        return target_shards.pop()
    if not target_shards:
        raise ValueError(f"cannot assign project source to a shard: {relative.as_posix()}")
    names = ", ".join(sorted(target_shards))
    raise ValueError(f"project source has conflicting shard targets ({names}): {relative.as_posix()}")


def source_shards(
    database: list[dict], root: pathlib.Path, match: str | None = None
) -> dict[str, set[pathlib.Path]]:
    """Return the complete, disjoint source matrix keyed by shard name."""
    records: dict[pathlib.Path, tuple[pathlib.PurePosixPath, set[str | None]]] = {}
    for entry in database:
        source_info = _relative_source(entry["file"], root)
        if source_info is None:
            continue
        source, relative = source_info
        if not _is_c_cpp_translation_unit(source):
            continue
        if not relative.parts or relative.parts[0] not in {"src", "tests"}:
            continue
        record = records.setdefault(source, (relative, set()))
        record[1].add(extract_target(entry.get("output")))

    selected = {shard: set() for shard in SHARDS}
    match_pattern = re.compile(match) if match is not None else None
    for source, (relative, targets) in records.items():
        if match_pattern is not None and match_pattern.search(relative.as_posix()) is None:
            continue
        shard = classify_source(relative, targets)
        selected[shard].add(source)
    return selected


def _format_source(source: pathlib.Path, root: pathlib.Path) -> str:
    return source.relative_to(root).as_posix()


def main() -> int:
    args = parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build_dir = args.build_dir.resolve()
    database_path = build_dir / "compile_commands.json"
    with database_path.open(encoding="utf-8") as database_file:
        database = json.load(database_file)

    try:
        matrix = source_shards(database, root, args.match)
    except ValueError as error:
        print(f"clang-tidy source classification failed: {error}", file=sys.stderr)
        return 2

    sources = matrix[args.shard] if args.shard is not None else set().union(*matrix.values())
    if args.list:
        selected_name = args.shard or "all"
        print(f"clang-tidy {selected_name}: {len(sources)} project translation units")
        for source in sorted(sources):
            print(_format_source(source, root))
        return 0

    def check(source: pathlib.Path) -> tuple[pathlib.Path, subprocess.CompletedProcess[str]]:
        root_pattern = re.escape(root.as_posix())
        header_filter = rf"^{root_pattern}/(?:src|tests)/"
        command = [args.clang_tidy, "--quiet", f"--header-filter={header_filter}", "-p", str(build_dir), str(source)]
        if args.fix:
            command.append("--fix-errors")
        return source, subprocess.run(command, capture_output=True, text=True, check=False)

    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for source, result in executor.map(check, sorted(sources)):
            if result.returncode != 0:
                failures.append(source)
                if args.summary:
                    diagnostics = sorted(set(re.findall(r"\[([^,\]]+)(?:,-warnings-as-errors)?\]", result.stdout)))
                    print(f"{_format_source(source, root)}: {', '.join(diagnostics)}")
                else:
                    sys.stdout.write(result.stdout)
                    sys.stderr.write(result.stderr)

    if failures:
        print(f"clang-tidy failed for {len(failures)} of {len(sources)} project translation units", file=sys.stderr)
        return 1
    print(f"clang-tidy passed for {len(sources)} project translation units")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
