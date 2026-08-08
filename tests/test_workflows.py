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

if __name__ == "__main__":
    unittest.main()
