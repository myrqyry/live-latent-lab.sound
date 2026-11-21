"""Guard public OBS Sound repository links against renamed/dev destinations."""
from pathlib import Path
import json
import unittest

ROOT = Path(__file__).resolve().parents[1]
URL = "https://github.com/myrqyry/live-latent-lab.sound"


class PublicRepositoryLinkTests(unittest.TestCase):
    def test_release_metadata_uses_publication_repository(self):
        for relative in ("buildspec.json", "data/manifest.json"):
            with self.subTest(file=relative):
                info = json.loads((ROOT / relative).read_text())
                self.assertEqual(info["website"], URL)

    def test_obs_menu_targets_publication_repository(self):
        source = (ROOT / "src/optimizer_ui.cpp").read_text()
        self.assertIn(f'QUrl("{URL}")', source)
        self.assertIn(f'QUrl("{URL}/releases")', source)
        self.assertNotIn("https://github.com/myrqyry/live-latent-lab-sound", source)


if __name__ == "__main__":
    unittest.main()
