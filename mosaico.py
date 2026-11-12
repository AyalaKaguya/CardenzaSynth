#!/usr/bin/env python3
"""Use the product CLI supplied by the pinned Mosaico Vibe dependency."""
from pathlib import Path
import sys

if sys.version_info < (3, 10):
    raise SystemExit("Cardenza Mosaico tooling requires Python 3.10 or newer")

TOOLS_ROOT = (Path(__file__).resolve().parent / "tools/mosaico-vibe/submodule/esp-mosaico-utils/mosaico-tools")
if not (TOOLS_ROOT / "tools/mosaico_cli/cli.py").is_file():
    raise SystemExit("Run: git submodule update --init tools/mosaico-vibe; then git -C tools/mosaico-vibe submodule update --init submodule/esp-mosaico-utils submodule/esp-mosaico-bsp")
sys.path.insert(0, str(TOOLS_ROOT / "tools"))
from mosaico_cli.cli import main

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "native-files":
        from tools.mosaico_files import main as native_files_main
        raise SystemExit(native_files_main(sys.argv[2:], tool_root=TOOLS_ROOT))
    if len(sys.argv) > 1 and sys.argv[1] == "native-touch":
        from tools.mosaico_touch import main as native_touch_main
        raise SystemExit(native_touch_main(sys.argv[2:], tool_root=TOOLS_ROOT))
    raise SystemExit(main(tool_root=TOOLS_ROOT))
