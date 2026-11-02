"""Check the actual generated payloads without a firmware or browser dependency."""
import gzip
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("web_build", ROOT / "web/build.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class WebAssetsTest(unittest.TestCase):
    def test_recovery_pages_keep_their_standalone_actions(self):
        for filename, action in (("reset.html", "/reset"), ("reboot.html", "/reboot"),
                                 ("update.html", "/update")):
            with self.subTest(page=filename):
                html = builder.page_bytes(ROOT / "web/pages" / filename).decode("utf-8")
                self.assertIn("action='%s'" % action, html)
                self.assertRegex(html, r"method='(?:post|POST)'")
                self.assertNotRegex(html, r"<script[^>]+src=|<link[^>]+stylesheet")
        update = builder.page_bytes(ROOT / "web/pages/update.html").decode("utf-8")
        self.assertIn("enctype='multipart/form-data'", update)
        self.assertIn("name='firmware'", update)

    def test_generated_pages_decompress_to_the_complete_sources(self):
        builder.build(ROOT)
        header = (ROOT / "OurBrewbot/WebUIAssets.h").read_text()
        for symbol, filename in builder.PAGES:
            with self.subTest(page=filename):
                match = re.search(r"uint8_t " + symbol + r"\[\] PROGMEM = \{(.*?)\};", header, re.S)
                self.assertIsNotNone(match)
                data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", match[1]))
                self.assertEqual(data[:3], b"\x1f\x8b\x08")
                self.assertEqual(data[4:8], b"\0\0\0\0")
                self.assertEqual(gzip.decompress(data), builder.prepared_page(ROOT, filename, builder.shared_assets(ROOT)))

    def test_shared_paths_match_the_shipped_content(self):
        builder.build(ROOT)
        header = (ROOT / "OurBrewbot/WebUIAssets.h").read_text()
        assets = builder.shared_assets(ROOT)
        admin = builder.prepared_page(ROOT, "admin.html", assets)
        for symbol, path, raw, compressed in assets:
            self.assertEqual(len(path), 10)
            self.assertIn(('#define %s_PATH "%s"' % (symbol, path)), header)
            self.assertIn(path.encode(), admin)
            self.assertEqual(gzip.decompress(compressed), raw)
        self.assertNotIn(b"{{", admin)

    def test_shared_paths_follow_text_changes_instead_of_gzip_encoding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "web/styles").mkdir(parents=True)
            (root / "web/js").mkdir()
            css = root / "web/styles/admin.css"
            css.write_text("body { color: red; }", encoding="utf-8")
            for name in builder.SCRIPTS:
                (root / "web/js" / name).write_text("/* example script */", encoding="utf-8")

            original = builder.shared_assets(root)
            compress = gzip.compress
            # Different compressor versions can encode the same text differently.
            with patch.object(builder.gzip, "compress", side_effect=lambda data, **options:
                              compress(data, compresslevel=1, mtime=0)):
                encoded = builder.shared_assets(root)
            for before, after in zip(original, encoded):
                self.assertEqual(before[1], after[1])
                self.assertNotEqual(before[3], after[3])
                self.assertEqual(gzip.decompress(after[3]), before[2])

            css.write_text("body { color: blue; }", encoding="utf-8")
            changed = builder.shared_assets(root)
            self.assertNotEqual(original[0][1], changed[0][1])
            self.assertEqual(original[1][1], changed[1][1])

    def test_preparation_keeps_significant_whitespace(self):
        with tempfile.TemporaryDirectory() as directory:
            page = Path(directory) / "page.html"
            page.write_bytes(b"<pre>\r\n  keep this\r\n\r\n</pre><script>let s=`\r\n  also this\r\n`;</script>")
            self.assertEqual(builder.page_bytes(page), b"<pre>\n  keep this\n\n</pre><script>let s=`\n  also this\n`;</script>")

    def test_unchanged_generation_leaves_the_header_alone(self):
        builder.build(ROOT)
        header = ROOT / "OurBrewbot/WebUIAssets.h"
        previous = header.stat().st_mtime_ns
        builder.build(ROOT)
        self.assertEqual(header.stat().st_mtime_ns, previous)


if __name__ == "__main__":
    unittest.main()
