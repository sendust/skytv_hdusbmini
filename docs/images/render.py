#!/usr/bin/env python3
"""Render docs/images/src/*.html to docs/images/*.png (needs: pip install playwright; playwright install chromium)."""
import pathlib
from playwright.sync_api import sync_playwright

here = pathlib.Path(__file__).resolve().parent
with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page(viewport={'width': 1200, 'height': 400}, device_scale_factor=1.5)
    for src in sorted((here / 'src').glob('*.html')):
        pg.goto(src.as_uri())
        pg.wait_for_timeout(200)
        out = here / (src.stem + '.png')
        el = pg.locator('svg').first if pg.locator('svg').count() else pg.locator('.wrap').first
        el.screenshot(path=str(out))
        print(out.name)
    b.close()
