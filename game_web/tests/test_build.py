"""Exercise the actual root argument parsing with isolated launch stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BuildTests(unittest.TestCase):
    def test_dispatch(self):
        source = (ROOT / 'build.sh').read_text()
        overrides = '''
prepare_flatbuffers_and_schemas() { echo SCHEMAS; }
run_blender_side_build_and_launch() { echo "BLENDER:$BLENDER_BUILD_MODE:$run_args"; }
package_extension() { echo PACKAGE; }
start_parallel_branch() { shift; "$@"; }
wait_for_parallel_branches() { :; }
'''
        source = source.replace('while [[ $# -gt 0 ]]; do', overrides + '\nwhile [[ $# -gt 0 ]]; do')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'build.sh').write_text(source)
            for name in ('game', 'game_web'):
                (root / name).mkdir()
                launcher = root / name / 'build.sh'
                launcher.write_text('#!/bin/bash\necho RENDERER:' + name + ':"$@"\n')
                launcher.chmod(0o755)
            cases = [([], 'game', 'native'), (['-g'], 'game', None),
                     (['-web'], 'game_web', 'native'), (['-g', '-web'], 'game_web', None),
                     (['-python', '--web', '-f', 'example.blend'], 'game_web', 'python')]
            for args, renderer, blender in cases:
                with self.subTest(args=args):
                    result = subprocess.run(['bash', str(root / 'build.sh'), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('RENDERER:' + renderer + ':', result.stdout)
                    self.assertEqual('BLENDER:' in result.stdout, blender is not None)
                    if blender:
                        self.assertIn('BLENDER:' + blender, result.stdout)
                    if '-f' in args:
                        self.assertIn('/example.blend', result.stdout)
            result = subprocess.run(['bash', str(root / 'build.sh'), '-web', '--package-only'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('PACKAGE', result.stdout)
            self.assertNotIn('RENDERER:', result.stdout)
            result = subprocess.run(['bash', str(root / 'build.sh'), '-web', '-screenshot', 'captures'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('SCHEMAS', result.stdout)
            self.assertFalse((root / 'captures').exists())
            import os
            result = subprocess.run(['bash', str(root / 'build.sh'), '-g', '-web'], capture_output=True, text=True,
                                    env={**os.environ, 'BLENDER_LIVE_LINK_SKIP_GAME': '1'})
            self.assertEqual(result.returncode, 0)
            self.assertNotIn('RENDERER:', result.stdout)

    def test_packaging_excludes_web(self):
        self.assertIn('"$BASE_DIR/game_web/*"', (ROOT / 'build.sh').read_text())
