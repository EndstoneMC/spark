import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WorkflowTest(unittest.TestCase):
    def test_build_enforces_project_quality(self):
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertEqual(workflow.count("conanfile.py"), 2)
        self.assertEqual(workflow.count("tools/run_clang_tidy.py"), 2)
        self.assertEqual(workflow.count("--dry-run --Werror"), 2)
        self.assertEqual(workflow.count("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"), 2)

    def test_release_is_published_after_both_artifacts(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertNotIn("gh release upload", workflow)
        self.assertEqual(workflow.count("gh release create"), 1)
        self.assertIn("needs: [release, windows, linux]", workflow)
        self.assertLess(workflow.index("  windows:"), workflow.index("  publish:"))
        self.assertLess(workflow.index("  linux:"), workflow.index("  publish:"))
        self.assertIn("ctest --test-dir build/RelWithDebInfo --output-on-failure", workflow)

if __name__ == "__main__":
    unittest.main()
