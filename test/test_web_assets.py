"""Check the actual generated payloads without a firmware or browser dependency."""
import gzip
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

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
                self.assertEqual(gzip.decompress(data), builder.page_bytes(ROOT / "web/pages" / filename))

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
