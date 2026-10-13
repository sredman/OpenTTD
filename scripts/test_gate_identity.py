#!/usr/bin/env python3
"""Automated verification harness for gate identity, inventory checks, and failure rejection."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

def run_gate_tests(repo_dir: Path):
    print("Testing gate identity and rejection harness on:", repo_dir)
    with tempfile.TemporaryDirectory(prefix="gate-harness-", dir=Path.cwd()) as tmp:
        root = Path(tmp)
        scripts = root / "scripts"
        scripts.mkdir()
        for name in ("gate.sh", "required_tests.txt"):
            shutil.copy2(repo_dir / "scripts" / name, scripts / name)
        
        build = root / "build"
        (build / "regression").mkdir(parents=True)
        (build / "game" / "aircraft").mkdir(parents=True)
        for name in ("openttd_test", "regression/regression.cfg", "game/aircraft/test.sav"):
            (build / name).touch()
        
        # Dedicated-only mock executable
        (build / "openttd").write_text("#!/bin/sh\nif [ \"$1\" = -h ]; then printf 'List of video drivers:\\n dedicated\\n'; fi\nexit 0\n")
        (build / "openttd").chmod(0o755)

        fakebin = root / "fakebin"
        fakebin.mkdir()
        (fakebin / "cmake").write_text("#!/bin/sh\nexit 0\n")
        (fakebin / "cmake").chmod(0o755)

        (fakebin / "ctest").write_text("""#!/usr/bin/env python3
import os
from pathlib import Path
import sys

names = Path(os.environ['REVIEW_MANIFEST']).read_text().splitlines()
case = os.environ['REVIEW_CASE']

if case == 'missing':
    names.remove('FixSCCEncoded')
    names.append('Unrelated extra test')

if '-N' in sys.argv:
    for i, name in enumerate(names, 1):
        print(f'  Test #{i}: {name}')
    print(f'Total Tests: {len(names)}')
else:
    for i, name in enumerate(names, 1):
        if case == 'skipped' and name == 'FixSCCEncoded':
            status = '***Skipped'
        elif case == 'disabled' and name == 'FixSCCEncoded':
            status = 'Disabled'
        elif case == 'failed' and name == 'FixSCCEncoded':
            status = '***Failed'
        else:
            status = 'Passed'
        print(f'{i}/{len(names)} Test #{i}: {name} ............ {status} 0.01 sec')
    if case in ('skipped', 'disabled', 'failed'):
        print(f'98% tests passed, 1 tests failed out of {len(names)}')
    else:
        print(f'100% tests passed, 0 tests failed out of {len(names)}')
""")
        (fakebin / "ctest").chmod(0o755)

        env = dict(os.environ, PATH=str(fakebin) + os.pathsep + os.environ['PATH'], REVIEW_MANIFEST=str(scripts / "required_tests.txt"))

        cases = {
            'clean': 0,
            'missing': 1,
            'skipped': 1,
            'disabled': 1,
            'failed': 1,
        }

        for case, expected in cases.items():
            result = subprocess.run(
                ['bash', str(scripts / 'gate.sh'), '--mode', 'dedicated', str(build)],
                env=dict(env, REVIEW_CASE=case),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT
            )
            actual = result.returncode
            status_str = "PASS" if (actual == expected or (expected != 0 and actual != 0)) else "FAIL"
            print(f"[{status_str}] case '{case}': actual={actual}, expected={expected}")
            if status_str == "FAIL":
                print("Output:\n", result.stdout)
                sys.exit(1)

        # Negative test: graphical mode on dedicated binary must fail
        gui_result = subprocess.run(
            ['bash', str(scripts / 'gate.sh'), '--mode', 'graphical', str(build)],
            env=dict(env, REVIEW_CASE='clean'),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT
        )
        gui_status_str = "PASS" if gui_result.returncode != 0 else "FAIL"
        print(f"[{gui_status_str}] case 'graphical_on_dedicated_binary': actual={gui_result.returncode}, expected=nonzero")
        if gui_status_str == "FAIL":
            print("Output:\n", gui_result.stdout)
            sys.exit(1)

        print("All gate identity and mode rejection tests passed successfully.")

if __name__ == "__main__":
    target = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    run_gate_tests(target)
