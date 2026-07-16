import unittest

from release_changelog import render_release


HEADER = """# Changelog

## [Unreleased]
"""


class ReleaseChangelogTest(unittest.TestCase):
    def test_creates_new_release_section(self):
        source = HEADER + """
### Fixed

- New fix.

## [0.1.0] - 2026-01-01

### Added

- Initial release.

[Unreleased]: old
[0.1.0]: old
"""
        changelog, notes = render_release(source, "0.1.1", "2026-07-17", "EndstoneMC/spark")
        self.assertEqual(changelog.count("## [0.1.1]"), 1)
        self.assertEqual(changelog.count("[0.1.1]:"), 1)
        self.assertIn("- New fix.", notes)
        self.assertIn("compare/v0.1.0...v0.1.1", changelog)

    def test_merges_existing_release_without_duplicates(self):
        source = HEADER + """
### Fixed

- New fix.

## [0.1.0] - 2026-01-01

### Added

- Initial release.

## [0.1.0] - 2026-01-02

### Fixed

- New fix.

[Unreleased]: wrong
[0.1.0]: wrong
[0.1.0]: duplicate
"""
        changelog, notes = render_release(source, "0.1.0", "2026-07-17", "EndstoneMC/spark")
        self.assertEqual(changelog.count("## [0.1.0]"), 1)
        self.assertEqual(changelog.count("[0.1.0]:"), 1)
        self.assertIn("### Fixed", notes)
        self.assertIn("### Added", notes)
        self.assertIn("EndstoneMC/spark/releases/tag/v0.1.0", changelog)

    def test_deduplicates_identical_category_content(self):
        content = """
### Fixed

- Same fix.
"""
        source = HEADER + content + "\n## [0.1.0] - 2026-01-01\n" + content
        changelog, notes = render_release(source, "0.1.0", "2026-07-17", "EndstoneMC/spark")
        self.assertEqual(notes.count("- Same fix."), 1)
        self.assertEqual(changelog.count("## [0.1.0]"), 1)

    def test_rejects_empty_release(self):
        with self.assertRaisesRegex(ValueError, "release notes are empty"):
            render_release(HEADER, "0.1.0", "2026-07-17", "EndstoneMC/spark")


if __name__ == "__main__":
    unittest.main()
