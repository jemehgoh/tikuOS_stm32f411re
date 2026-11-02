"""Host-side acceptance tests for the STM32N6 raw model packager."""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stm32n6_relpack as relpack


ROOT = Path(__file__).resolve().parents[2]
MODEL = ROOT / "data" / "npu" / "network_rel.bin"
SCRIPT = ROOT / "tools" / "npu" / "stm32n6_relpack.py"


class RawRelpackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.image = MODEL.read_bytes()

    def test_validation_reports_raw_length_and_crc(self) -> None:
        metadata = relpack.validate(self.image)
        self.assertEqual(self.image[:4], struct.pack("<I", relpack.MAGIC))
        self.assertEqual(metadata["bytes"], len(self.image))
        self.assertEqual(metadata["crc32"], relpack.crc32(self.image))

    def test_cli_preserves_raw_bytes_and_emits_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_root = Path(directory) / "data" / "npu"
            header = Path(directory) / "tiku_n6_model_manifest.h"
            subprocess.run(
                [sys.executable, str(SCRIPT), str(MODEL),
                 "--output-root", str(output_root), "--name", "network_rel.bin",
                 "--manifest-header", str(header)],
                cwd=ROOT, check=True)

            self.assertEqual((output_root / "network_rel.bin").read_bytes(),
                             self.image)
            manifest = json.loads(
                (output_root / "network_rel.bin.manifest.json").read_text())
            self.assertEqual(manifest["storage"], "raw-slot")
            self.assertEqual(manifest["raw_offset"], 0)
            self.assertEqual(manifest["payload_offset"], 0)
            self.assertIsNone(manifest["file_params_ptr"])
            self.assertEqual(manifest["bytes"], len(self.image))
            self.assertEqual(manifest["crc32"], relpack.crc32(self.image))
            self.assertIn("TIKU_NPU_MODEL_FILE_PARAMS_PTR 0U",
                          header.read_text())

    def test_split_image_is_rejected(self) -> None:
        image = bytearray(self.image)
        struct.pack_into("<I", image, 12 * 4, 0)
        with self.assertRaisesRegex(ValueError, "split"):
            relpack.validate(bytes(image))

    def test_slot_overflow_is_rejected(self) -> None:
        with mock.patch.object(relpack, "OSPI_MODEL_BYTES",
                               len(self.image) - 1):
            with self.assertRaisesRegex(ValueError, "raw N6 model slot"):
                relpack.validate(self.image)


if __name__ == "__main__":
    unittest.main()
