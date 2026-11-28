import importlib.util
import os
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
spec=importlib.util.spec_from_file_location("sound_stems",Path(__file__).resolve().parents[1]/"tools/sound-stems.py")
s=importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)

class TestProvision(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.cfg=self.root/"stems.json"
        self.python=self.root/"python"
        self.python.touch(mode=0o700)
        self.models=self.root/"models with spaces"
        self.models.mkdir()
        self.mel=self.root/"melband.safetensors"
        self.mel.touch()
        self.values={"python":str(self.python),"demucs_models":str(self.models),"melband_model":str(self.mel)}
    def test_config_preserves_existing(self):
        s.write_config(self.cfg,self.values)
        self.assertEqual(s.read_config(self.cfg),self.values)
        self.assertEqual(self.cfg.stat().st_mode&0o777,0o600)
        with self.assertRaises(FileExistsError):s.write_config(self.cfg,{"python":str(self.root/"other")})
        self.assertEqual(s.read_config(self.cfg),self.values)
    def test_reject_symlink_and_unknown_keys(self):
        target=self.root/"target";target.write_text("{}")
        self.cfg.symlink_to(target)
        with self.assertRaises(ValueError):s.read_config(self.cfg)
        with self.assertRaises(ValueError):s.write_config(self.cfg,self.values)
        self.cfg.unlink()
        with self.assertRaises(ValueError):s.write_config(self.cfg,{"command":"anything"})
    def test_explicit_profiles(self):
        with patch.dict(os.environ,{"XDG_RUNTIME_DIR":str(self.root)}):
            a=s.command(self.values,"demucs")
            b=s.command(self.values,"melband")
            self.assertEqual(a[0],str(self.python))
            self.assertIn(str(self.models),a)
            self.assertIn("resident",a);self.assertIn("cuda",a)
            self.assertIn(str(self.root/"lll-sound-stems.sock"),a)
            self.assertIn(str(self.mel),b)
            self.assertIn(str(self.root/"lll-sound-melband.sock"),b)
            self.assertNotIn("resident",b)
            with self.assertRaises(ValueError):s.command(self.values,"unknown")
    def test_missing_runtime_and_occupied_socket(self):
        with patch.dict(os.environ,{"XDG_RUNTIME_DIR":""}):
            with self.assertRaises(ValueError):s.command(self.values,"melband")
        with patch.dict(os.environ,{"XDG_RUNTIME_DIR":str(self.root)}):
            (self.root/"lll-sound-melband.sock").touch()
            with self.assertRaises(FileExistsError):s.command(self.values,"melband")
    def test_explicit_safetensors_only_for_melband(self):
        self.values["safetensors_package"]=str(self.root/"safetensors")
        with patch.dict(os.environ,{"XDG_RUNTIME_DIR":str(self.root)}):
            self.assertIn("--safetensors-package",s.command(self.values,"melband"))
            self.assertNotIn("--safetensors-package",s.command(self.values,"demucs"))
    def test_private_managed_endpoint(self):
        with patch.dict(os.environ,{"XDG_RUNTIME_DIR":""}):
            private=str(self.root/"private.sock")
            for profile in ("demucs","melband"):
                args=s.command(self.values,profile,private)
                self.assertEqual(args[args.index("--socket")+1],private)
            with self.assertRaises(ValueError):s.command(self.values,"unknown",private)
            Path(private).touch()
            with self.assertRaises(FileExistsError):s.command(self.values,"melband",private)
if __name__=="__main__":unittest.main()
