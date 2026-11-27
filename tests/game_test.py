#!/usr/bin/env python3
"""Game tests: run vgrun on the player's game folder (VOY_GAME_DIR) and check
the result. Skipped (exit 77) without a game folder.

  attract   1500 frames twice: no fail-closed stop, both runs end on the same
            frame hash (virtual time makes runs deterministic)
  play      a scripted coin, start, shoot and reload game reaches gameplay
            (frames with thousands of vertices) without a stop
  service   the test switch runs the service menu (stvserv)
"""
import os
import re
import subprocess
import sys
import tempfile


def run(vgrun, game, frames, extra=()):
    with tempfile.TemporaryDirectory() as container:
        cmd = [vgrun, '--content', game, '--container', container, '--frames', str(frames), '--hash-every', '100']
        cmd += list(extra)
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        return p.returncode, p.stdout, p.stderr


def main():
    vgrun, test = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    game = os.environ.get('VOY_GAME_DIR', '')
    if not game or not os.path.isfile(os.path.join(game, 'trek')):
        print('VOY_GAME_DIR is not set to a game folder: skipped')
        return 77
    if test == 'attract':
        a = run(vgrun, game, 1500)
        b = run(vgrun, game, 1500)
        for code, out, err in (a, b):
            if code != 0 or 'fail-closed' in err:
                print(out, err)
                return 1
        ha = re.search(r'last frame hash (\w+)', a[1]).group(1)
        hb = re.search(r'last frame hash (\w+)', b[1]).group(1)
        print('hashes', ha, hb)
        if ha != hb:
            return 1
        if 'after 1500 frames' not in a[1]:
            return 1
        return 0
    if test == 'play':
        code, out, err = run(vgrun, game, 900, ['--script', os.path.join(here, 'scripts', 'play.txt')])
        if code != 0 or 'fail-closed' in err:
            print(out, err)
            return 1
        big = [int(m) for m in re.findall(r'frame (\d+): (\d+) vertices', out) for m in [m[1]]]
        print('vertices per sampled frame:', big)
        return 0 if max(big) > 5000 else 1
    if test == 'service':
        code, out, err = run(vgrun, game, 400, ['--script', os.path.join(here, 'scripts', 'service.txt'), '--stdout'])
        if code != 0 or 'fail-closed' in err:
            print(out, err)
            return 1
        ok = 'test pressed' in err and '[stvserv]' in err
        print('service menu reached' if ok else 'service menu not reached')
        return 0 if ok else 1
    print('unknown test', test)
    return 2


if __name__ == '__main__':
    sys.exit(main())
