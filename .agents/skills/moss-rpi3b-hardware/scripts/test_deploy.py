"""Run a relocated wrapper against fake checkouts; no serial or JTAG connections."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class DeployTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        self.root = self.base / "other developer" / "仓库 with spaces"
        (self.root / "scripts").mkdir(parents=True)
        (self.root / "scripts/artifacts.py").write_text("CHECKOUT_TOKEN = 'selected checkout'\n", encoding="utf-8")
        (self.root / "board tree.dtb").write_bytes(b"fake DTB fixture")
        # Distinct rates exercise both clamping and retaining an already lower
        # rate. They are test inputs, not hardware configuration recommendations.
        (self.root / "scripts/rpi.py").write_text(
            "import sys\n"
            "from pathlib import Path\n"
            "from scripts.artifacts import CHECKOUT_TOKEN\n"
            "class OpenOCD:\n"
            "    def command(self, text): return text\n"
            "def main():\n"
            "    assert CHECKOUT_TOKEN == 'selected checkout'\n"
            "    assert Path.cwd() == Path(__file__).parent.parent\n"
            "    assert sys.argv[1:] == ['deploy', '--dtb', 'board tree.dtb', '--check']\n"
            "    assert Path(sys.argv[3]).read_bytes() == b'fake DTB fixture'\n"
            "    ocd = OpenOCD()\n"
            "    assert ocd.command('adapter speed 4000') == 'adapter speed 1500'\n"
            "    assert ocd.command('adapter speed 1000') == 'adapter speed 1000'\n"
            "    assert ocd.command('targets bcm2837.cpu0') == 'targets bcm2837.cpu0'\n"
            "    return 0\n",
            encoding="utf-8",
        )
        self.external = self.base / "shared skills" / "renamed skill" / "deploy.py"
        self.external.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).with_name("deploy.py"), self.external)

    def invoke(self, wrapper, cwd, *options):
        return subprocess.run(
            [sys.executable, "-B", str(wrapper), *options, "--", "--dtb", "board tree.dtb", "--check"],
            cwd=cwd,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_external_skill_explicit_repo_from_unrelated_directory(self):
        result = self.invoke(self.external, self.base, "--repo", str(self.root), "--jtag-khz", "1500")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_discover_repo_from_nested_working_directory(self):
        nested = self.root / "src" / "nested"
        nested.mkdir(parents=True)
        result = self.invoke(self.external, nested, "--jtag-khz", "1500")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_discover_repo_from_relocated_embedded_skill(self):
        embedded = self.root / "custom tools" / "skill" / "deploy.py"
        embedded.parent.mkdir(parents=True)
        shutil.copyfile(self.external, embedded)
        result = self.invoke(embedded, self.base, "--jtag-khz", "1500")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_explicit_repo_does_not_fall_back_to_working_directory(self):
        result = self.invoke(self.external, self.root, "--repo", str(self.base), "--jtag-khz", "1500")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Moss checkout not found", result.stderr)

    def test_speed_must_be_explicit_and_positive(self):
        for options in ([], ["--jtag-khz", "0"], ["--jtag-khz", "-1"]):
            with self.subTest(options=options):
                result = self.invoke(self.external, self.root, *options)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--jtag-khz", result.stderr)


if __name__ == "__main__":
    unittest.main()
