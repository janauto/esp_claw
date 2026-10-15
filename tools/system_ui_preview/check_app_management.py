"""Exercise the real LVGL app UI with synthetic package descriptors (no device)."""
from pathlib import Path
import os
import subprocess
import sys

folder = Path(__file__).resolve().parent
binary = folder/'build/system_ui_preview'
output = Path(sys.argv[1]) if len(sys.argv) > 1 else folder/'build/app-management-check'
output.mkdir(parents=True, exist_ok=True)
cases = {
    'home': (275, [], None),
    'menu': (9, ['240,406,800'], None),
    'rename': (9, ['240,406,800', '240,565,80'], None),
    'rename-save': (9, ['240,406,800', '240,565,80', '100,475,80', '345,381,80'], 'PREVIEW_RENAME page=9 id=app:demo0 title=应用 1w'),
    'rename-cancel': (9, ['240,406,800', '240,565,80', '135,381,80'], None),
    'confirm': (9, ['240,406,800', '240,505,80'], None),
    'uninstall-confirm': (9, ['240,406,800', '240,505,80', '240,625,80'], 'PREVIEW_ACTION page=9 id=app:demo0 action=0'),
    'uninstall-cancel': (9, ['240,406,800', '240,505,80', '240,695,80'], None),
    'uninstall-large-index': (275, ['240,406,800', '240,505,80', '240,625,80'], 'PREVIEW_ACTION page=275 id=app:demo266 action=0'),
    'system-protected': (0, ['240,406,800', '240,505,80'], None),
}
for name, (page, taps, expected) in cases.items():
    result = subprocess.run([str(binary), '480', '800', f'home:{page}', str(output/(name+'.bmp')), *taps],
                            env={**os.environ, 'PREVIEW_PACKAGE_COUNT':'267'}, text=True, capture_output=True, check=True)
    log = result.stdout + result.stderr
    assert f'PREVIEW_HOME page={page} total=276' in log, (name, log[-500:])
    events = [line for line in log.splitlines() if line.startswith(('PREVIEW_RENAME', 'PREVIEW_ACTION'))]
    assert events == ([expected] if expected else []), (name, events)
    (output/(name+'.log')).write_text(log)
    print(f'PASS {name}')
