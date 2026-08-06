#!/usr/bin/env python3

from __future__ import annotations

import unittest

from convert_rgb565_framebuffer import rgb565_le_to_rgb


class Rgb565FramebufferTests(unittest.TestCase):
    def test_primary_colors_expand_to_full_rgb(self) -> None:
        source = bytes.fromhex("00f8e0071f00ffffffff0000")
        rgb, unique_colors, non_black = rgb565_le_to_rgb(source, 3, 2)
        self.assertEqual(
            rgb,
            bytes(
                (
                    255, 0, 0,
                    0, 255, 0,
                    0, 0, 255,
                    255, 255, 255,
                    255, 255, 255,
                    0, 0, 0,
                )
            ),
        )
        self.assertEqual(unique_colors, 5)
        self.assertEqual(non_black, 5)

    def test_rejects_mismatched_frame_size(self) -> None:
        with self.assertRaisesRegex(ValueError, "expected 8 bytes, got 2"):
            rgb565_le_to_rgb(b"\x00\x00", 2, 2)

    def test_rejects_non_positive_dimensions(self) -> None:
        with self.assertRaisesRegex(ValueError, "must be positive"):
            rgb565_le_to_rgb(b"", 0, 1)


if __name__ == "__main__":
    unittest.main()
