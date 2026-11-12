#!/usr/bin/env python3
"""Replay a bounded touch batch through the native project's public Gateway RPC."""
from __future__ import annotations

import argparse
import base64
from dataclasses import replace
import json
from pathlib import Path
import re
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlsplit
from urllib.request import Request, build_opener
import uuid

from tools.mosaico_files import NoRedirect, identity


def actions(value: str) -> list[dict]:
    if len(value) > 65536:
        raise ValueError("Touch actions exceed 64 KiB")
    result = json.loads(value)
    if not isinstance(result, list) or not 1 <= len(result) <= 128:
        raise ValueError("Supply between 1 and 128 touch actions")
    duration = 0
    for action in result:
        if not isinstance(action, dict) or len(action) != 1:
            raise ValueError("Each action must contain one tap, hold or wait_ms")
        key, data = next(iter(action.items()))
        if key == "wait_ms":
            if type(data) is not int or not 0 <= data <= 1000:
                raise ValueError("wait_ms must be an integer from 0 to 1000")
            duration += data
            continue
        if key not in {"tap", "hold"} or not isinstance(data, list) or len(data) != (2 if key == "tap" else 3):
            raise ValueError("Use tap:[x,y] or hold:[x,y,milliseconds]")
        if any(type(item) is not int for item in data) or not all(0 <= item < 480 for item in data[:2]):
            raise ValueError("Touch coordinates must be integers from 0 to 479")
        held = 50 if key == "tap" else data[2]
        if not 10 <= held <= 2000:
            raise ValueError("hold duration must be between 10 and 2000 ms")
        duration += held + 50
    if duration > 30000:
        raise ValueError("Total touch batch duration must not exceed 30 seconds")
    return result


def request(scope, url: str, path: str, body: dict | None = None, *, operation_id=None):
    for lease in scope.leases.values():
        lease.check()
    headers = {"Content-Type": "application/json"}
    if operation_id:
        headers["X-Operation-ID"] = operation_id
    raw = json.dumps(body).encode("utf-8") if body is not None else None
    req = Request(url + path, data=raw, headers=headers, method="POST" if body is not None else "GET")
    try:
        with build_opener(NoRedirect()).open(req, timeout=8) as response:
            data = response.read(1024 * 1024 + 1)
            if len(data) > 1024 * 1024:
                raise RuntimeError("Gateway response exceeds 1 MiB")
            return json.loads(data)
    except HTTPError as error:
        raise RuntimeError(f"Gateway HTTP {error.code}: {error.read(8192).decode('utf-8', errors='replace')}") from error
    except (URLError, TimeoutError) as error:
        raise RuntimeError(f"Gateway request failed: {error}; the touch operation was not replayed") from error


def execute(args, scope, context) -> dict:
    from mosaico_cli.gateway import connected_devices, ensure_gateway, select_device
    from mosaico_cli.project import resolve_project

    project = resolve_project(context.workspace, args.project, Path.cwd())
    if not re.search(r"\bproject\(\s*CardenzaSynthMosaicoNative\s*\)", (project / "CMakeLists.txt").read_text(encoding="utf-8")):
        raise ValueError("native-touch requires the CardenzaSynthMosaicoNative project")
    args.project = str(project)
    session = ensure_gateway(context, None)
    select_device(connected_devices(context, session), args.device_id)
    if len(session.connection_args) != 2 or session.connection_args[0] != "--url":
        raise RuntimeError("native-touch requires the local project Gateway")
    url = session.connection_args[1]
    parsed = urlsplit(url)
    if parsed.scheme != "http" or parsed.hostname not in {"127.0.0.1", "localhost", "::1"} or parsed.username:
        raise RuntimeError("Unexpected project Gateway URL")
    prefix = "/v2/devices/" + quote(args.device_id, safe="")
    before = identity(request(scope, url, prefix), args.device_id)
    operations = []
    context.note("native-touch identity=" + json.dumps(before))

    def rpc(service: int, payload: bytes) -> dict:
        operation_id = str(uuid.uuid4())
        operations.append(operation_id)
        context.note(f"rpc operation_id={operation_id} service={service} payload={payload.hex()}")
        # Save IDs before transmission, so a lost response can be inspected
        # without repeating an action that may already have run.
        (context.directory / "operations.json").write_text(json.dumps(operations), encoding="utf-8")
        result = request(scope, url, prefix + "/rpc/raw", {
            "service_id": service, "method_id": 1, "deadline_ms": 1000,
            "payload_base64": base64.b64encode(payload).decode("ascii"),
        }, operation_id=operation_id)
        if result.get("operation", {}).get("status") != "succeeded":
            raise RuntimeError(f"RPC did not succeed; inspect operation {operation_id}")
        return result

    def point(kind: int, x: int, y: int) -> None:
        rpc(0x4344, bytes((kind, 128, x & 255, x >> 8, y & 255, y >> 8)))

    down = False
    last_x = last_y = 0
    completed = 0
    began = time.monotonic()
    try:
        for action in args.actions:
            if time.monotonic() - began > 30:
                raise RuntimeError("Touch batch exceeded 30 seconds; remaining actions were not sent")
            key, value = next(iter(action.items()))
            if key == "wait_ms":
                time.sleep(value / 1000)
            else:
                last_x, last_y = value[:2]
                down = True
                point(0, last_x, last_y)
                time.sleep((50 if key == "tap" else value[2]) / 1000)
                point(2, last_x, last_y)
                down = False
                time.sleep(0.05)
            completed += 1
        if identity(request(scope, url, prefix), args.device_id) != before:
            raise RuntimeError("Device boot/firmware changed during touch batch")
        diagnostics = rpc(0x4343, b"")
        encoded = diagnostics.get("payload_base64")
        if not isinstance(encoded, str):
            raise RuntimeError("Native diagnostics returned no payload")
        last_status = json.loads(base64.b64decode(encoded, validate=True))
        if identity(request(scope, url, prefix), args.device_id) != before:
            raise RuntimeError("Device boot/firmware changed during final diagnostics")
        return {"ok": True, "command": "native-touch", "device_id": args.device_id,
                "boot_id": before[1], "completed": completed, "diagnostics": last_status,
                "operation_ids": operations, "log": str(context.log_path)}
    finally:
        if down:
            try:
                if identity(request(scope, url, prefix), args.device_id) == before:
                    point(3, last_x, last_y)
                    context.note("active pointer cancelled after incomplete batch")
            except (OSError, RuntimeError, ValueError) as error:
                context.note(f"pointer cancellation could not be confirmed: {error}")


def main(argv=None, *, tool_root: Path) -> int:
    parser = argparse.ArgumentParser(prog="mosaico.py native-touch", description=__doc__)
    parser.add_argument("--project", default="Devices/MosaicoNative")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--actions", type=actions, required=True)
    parser.add_argument("--json", action="store_true", help="Output is always JSON")
    args = parser.parse_args(argv)
    from mosaico_cli.errors import MosaicoError
    from mosaico_cli.runtime import RunContext
    from mosaico_cli.session_runtime import CURRENT_SCOPE, SessionScope
    from mosaico_cli.workspace import load_workspace
    scope = SessionScope()
    args.command = "native-touch"
    args.public_command = "native-touch"
    args.json = True
    scope.arguments = args
    token = CURRENT_SCOPE.set(scope)
    context = None
    try:
        workspace = replace(load_workspace(tool_root), gateway_source_policy="exact")
        context = RunContext(workspace, "native-touch", json_output=True)
        result = execute(args, scope, context)
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (MosaicoError, OSError, RuntimeError, ValueError) as error:
        print(json.dumps({"ok": False, "command": "native-touch", "error": str(error),
                          "log": str(context.log_path) if context is not None else None}, ensure_ascii=False))
        return getattr(error, "exit_code", 5)
    finally:
        try:
            scope.close()
        finally:
            CURRENT_SCOPE.reset(token)


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    tools = root / "tools/mosaico-vibe/submodule/esp-mosaico-utils/mosaico-tools"
    sys.path.insert(0, str(tools / "tools"))
    raise SystemExit(main(tool_root=tools))
