#!/usr/bin/env python3
"""Build an offline Vibe Mode application bundle without connecting a device."""
from pathlib import Path
import argparse
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    tool_root = root / "tools/mosaico-vibe/submodule/esp-mosaico-utils/mosaico-tools"
    sys.path.insert(0, str(tool_root / "tools"))
    from mosaico_cli.errors import MosaicoError
    from mosaico_cli.gateway import ensure_iris_tools
    from mosaico_cli.runtime import RunContext, run_idf_target
    from mosaico_cli.workspace import load_workspace

    context = RunContext(load_workspace(tool_root, start=root), "offline-bundle")
    try:
        iris_python, _ = ensure_iris_tools(context)
        project = root / "Devices/Mosaico"
        run_idf_target(
            context,
            idf_path=arguments.idf_path.resolve(),
            project=project,
            build_dir=project / "build",
            target="system-update-bundle",
            definitions={"ESP_IRIS_PYTHON": str(iris_python)},
            timeout=1800,
        )
        bundle = project / "build/CardenzaSynthMosaico-system-update.irisfw"
        if not bundle.is_file() or bundle.stat().st_size == 0:
            raise RuntimeError("The build returned without a complete application bundle")
        print(f"Bundle: {bundle}")
        print(f"Log: {context.log_path}")
        return 0
    except (MosaicoError, OSError, RuntimeError) as error:
        print(f"Offline bundle failed: {error}\nLog: {context.log_path}", file=sys.stderr)
        return getattr(error, "exit_code", 1)


if __name__ == "__main__":
    raise SystemExit(main())
