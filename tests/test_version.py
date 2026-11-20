import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "version.py"


class ReleaseVersionTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.repo = work.name
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release@example.invalid")

    def git(self, *args, **kwargs):
        subprocess.run(["git", *args], cwd=self.repo, check=True, capture_output=True, **kwargs)

    def commit(self, date):
        env = {**os.environ, "GIT_AUTHOR_DATE": date, "GIT_COMMITTER_DATE": date}
        self.git("commit", "--allow-empty", "-m", date, env=env)

    def selected_version(self, head="main"):
        self.git("checkout", head)
        return subprocess.check_output([sys.executable, SCRIPT], cwd=self.repo, text=True).strip()

    def test_monthly_version_uses_commit_time_in_utc(self):
        self.commit("2026-11-01T00:30:00+08:00")
        self.assertEqual(self.selected_version(), "2026.10.1")
        self.git("tag", "2026.10.1")
        self.commit("2026-11-01T10:00:00+08:00")
        self.assertEqual(self.selected_version(), "2026.11.1")

    def test_increments_and_reuses_version_on_retry(self):
        self.commit("2026-11-02T00:00:00Z")
        self.git("tag", "2026.11.1")
        self.commit("2026-11-03T00:00:00Z")
        self.assertEqual(self.selected_version(), "2026.11.2")
        self.git("tag", "-a", "2026.11.2", "-m", "Release")
        self.assertEqual(self.selected_version(), "2026.11.2")

    def test_skips_an_unversioned_commit_with_a_released_descendant(self):
        self.commit("2026-11-02T00:00:00Z")
        self.git("branch", "older")
        self.commit("2026-11-03T00:00:00Z")
        self.git("tag", "-a", "2026.11.1", "-m", "Release")
        self.assertEqual(self.selected_version("older"), "")


if __name__ == "__main__":
    unittest.main()
