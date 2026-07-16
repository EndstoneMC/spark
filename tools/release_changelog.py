#!/usr/bin/env python3

import argparse
import re
from pathlib import Path


SECTION_RE = re.compile(r"^## \[([^]]+)](?: - ([^\n]+))?\s*$", re.MULTILINE)
SUBSECTION_RE = re.compile(r"^### (.+?)\s*$", re.MULTILINE)
REFERENCE_RE = re.compile(r"^\[([^]]+)]:\s+.*$", re.MULTILINE)
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")


def clean_block(value: str) -> str:
    return value.strip("\r\n ")


def split_subsections(value: str) -> tuple[str, list[tuple[str, str]]]:
    matches = list(SUBSECTION_RE.finditer(value))
    if not matches:
        return clean_block(value), []

    preamble = clean_block(value[: matches[0].start()])
    sections = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(value)
        sections.append((match.group(1), clean_block(value[match.end() : end])))
    return preamble, sections


def join_unique(first: str, second: str) -> str:
    first = clean_block(first)
    second = clean_block(second)
    if not first:
        return second
    if not second or first == second:
        return first
    return f"{first}\n\n{second}"


def merge_release_content(unreleased: str, existing: str) -> str:
    new_preamble, new_sections = split_subsections(unreleased)
    old_preamble, old_sections = split_subsections(existing)
    preamble = join_unique(new_preamble, old_preamble)

    order: list[str] = []
    contents: dict[str, str] = {}
    for heading, body in [*new_sections, *old_sections]:
        if heading not in contents:
            order.append(heading)
            contents[heading] = body
        else:
            contents[heading] = join_unique(contents[heading], body)

    parts = [preamble] if preamble else []
    for heading in order:
        body = contents[heading]
        parts.append(f"### {heading}" + (f"\n\n{body}" if body else ""))
    return "\n\n".join(parts)


def render_release(source: str, version: str, date: str, repository: str) -> tuple[str, str]:
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"invalid version: {version}")

    without_references = REFERENCE_RE.sub("", source).rstrip()
    matches = list(SECTION_RE.finditer(without_references))
    if not matches:
        raise ValueError("CHANGELOG.md has no version sections")

    prefix = without_references[: matches[0].start()].rstrip()
    sections: list[tuple[str, str | None, str]] = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(without_references)
        sections.append((match.group(1), match.group(2), clean_block(without_references[match.end() : end])))

    unreleased_sections = [section for section in sections if section[0] == "Unreleased"]
    target_sections = [section for section in sections if section[0] == version]
    if len(unreleased_sections) != 1:
        raise ValueError("CHANGELOG.md must contain exactly one Unreleased section")
    existing = ""
    for target_section in target_sections:
        existing = merge_release_content(existing, target_section[2])
    release_notes = merge_release_content(unreleased_sections[0][2], existing)
    if not release_notes:
        raise ValueError("release notes are empty")

    remaining = [section for section in sections if section[0] not in {"Unreleased", version}]
    for section_version, _, _ in remaining:
        if not VERSION_RE.fullmatch(section_version):
            raise ValueError(f"invalid changelog version section: {section_version}")

    output_parts = [prefix, "## [Unreleased]", f"## [{version}] - {date}\n\n{release_notes}"]
    for section_version, section_date, body in remaining:
        heading = f"## [{section_version}]"
        if section_date:
            heading += f" - {section_date}"
        output_parts.append(heading + (f"\n\n{body}" if body else ""))

    version_order = [version, *[section[0] for section in remaining]]
    references = [f"[Unreleased]: https://github.com/{repository}/compare/v{version}...HEAD"]
    for index, current in enumerate(version_order):
        if index + 1 < len(version_order):
            older = version_order[index + 1]
            references.append(f"[{current}]: https://github.com/{repository}/compare/v{older}...v{current}")
        else:
            references.append(f"[{current}]: https://github.com/{repository}/releases/tag/v{current}")

    changelog = "\n\n".join(output_parts) + "\n\n" + "\n".join(references) + "\n"
    return changelog, release_notes + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare Spark changelog and release notes")
    parser.add_argument("--version", required=True)
    parser.add_argument("--date", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--input", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--notes", type=Path, required=True)
    args = parser.parse_args()

    changelog, notes = render_release(
        args.input.read_text(encoding="utf-8"), args.version, args.date, args.repository
    )
    args.output.write_text(changelog, encoding="utf-8", newline="\n")
    args.notes.write_text(notes, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
