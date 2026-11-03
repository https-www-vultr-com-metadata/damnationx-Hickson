#!/usr/bin/env python3
"""Capture the three Cathedral references before building a release (requires Pillow)."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

REPO = Path(__file__).resolve().parents[2]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--data-dir', type=Path, required=True)
    parser.add_argument('--hd-dir', type=Path, required=True)
    parser.add_argument('--d2r-dir', type=Path, required=True)
    parser.add_argument('--rt', choices=['off', 'low', 'medium', 'high', 'ultra'], default='medium')
    parser.add_argument('--output', type=Path, default=REPO / 'host/damnationx_host/assets/brightness')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='damnationx-brightness-') as temporary:
        stage = Path(temporary)
        for name, mode in [('classic', 'classic'), ('hd', 'hd'), ('d2r', '3d')]:
            work = stage / name
            (work / 'config').mkdir(parents=True)
            (work / 'save').mkdir()
            capture = work / 'capture.png'
            script = work / 'capture.script'
            script.write_text('0 wait level 1\n3 time pause\n5 screenshot '
                              + json.dumps(str(capture)) + '\n6 exit\n')
            command = [str(args.binary.resolve()), '--brightness-preview', '--offscreen', '--mute',
                       '--no-time-travel', '--no-core-time-travel', '--hero', 'Brightness',
                       '--class', 'warrior', '--render-mode', mode, '--window', '960x540',
                       '--ui-size', '960x540', '--fps', 'off', '--script', str(script)]
            if name == 'classic':
                command += ['--hd', 'off', '--d2r', 'off', '--rt', 'off']
            elif name == 'hd':
                command += ['--hd-dir', str(args.hd_dir.resolve()), '--d2r', 'off', '--rt', 'off']
            else:
                command += ['--hd-dir', str(args.hd_dir.resolve()), '--d2r-dir', str(args.d2r_dir.resolve()),
                            '--d2r-models', 'on', '--d2r-env', 'on', '--rt', args.rt]
            command += ['--', '--diablo', '--data-dir', str(args.data_dir.resolve()),
                        '--config-dir', str(work / 'config'), '--save-dir', str(work / 'save')]
            env = dict(os.environ, DAMNATIONX_HOST_START_LEVEL='1', DAMNATIONX_HOST_GOD='1')
            with (work / 'capture.log').open('w') as log:
                try:
                    subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=150, check=True)
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                    print((work / 'capture.log').read_text(errors='replace')[-4000:])
                    raise
            with Image.open(capture) as image:
                if image.size != (960, 540):
                    raise ValueError(f'{name}: unexpected capture size {image.size}')
                # Remove HUD pixels; keep all profiles at the same display aspect.
                image.crop((0, 0, 960, 360)).convert('RGB').save(stage / (name + '.png'), optimize=True)
            print(f'Prepared {name}', flush=True)
        # Only replace the references after all three captures succeed.
        args.output.mkdir(parents=True, exist_ok=True)
        for name in ['classic', 'hd', 'd2r']:
            (args.output / (name + '.png')).write_bytes((stage / (name + '.png')).read_bytes())


if __name__ == '__main__':
    main()
