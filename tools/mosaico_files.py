#!/usr/bin/env python3
"""Read/write the native application's logical volume through its project Gateway."""
from __future__ import annotations

import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import uuid

MAX_BYTES = 32 * 1024 * 1024
PROJECT_NAME = "CardenzaSynthMosaicoNative"
VOLUME = "native"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise RuntimeError("Project Gateway redirected the request; no operation was replayed")


def relative_path(value: str) -> str:
    encoded = value.encode("utf-8")
    if (not value or len(encoded) > 255 or "\\" in value or ":" in value
            or any(part in {"", ".", ".."} for part in value.split("/"))
            or any(byte < 0x20 or byte == 0x7f for byte in encoded)):
        raise ValueError("Use a canonical application-relative file path of at most 255 UTF-8 bytes")
    return value


def identity(status: dict, device_id: str) -> tuple:
    status = status.get("device", status)
    if (status.get("device_id") != device_id or status.get("stale") is True
            or status.get("present") is False or status.get("firmware_mode") != "normal"
            or status.get("mode") != "develop" or status.get("project_name") != PROJECT_NAME):
        raise RuntimeError("Selected live device is not the native application in normal/develop mode")
    boot = status.get("boot_id_text") or status.get("boot_id")
    firmware = status.get("firmware_sha256")
    if boot in (None, "", 0, "0") or not isinstance(firmware, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", firmware):
        raise RuntimeError("Device boot/firmware identity is unavailable")
    return device_id, str(boot), firmware.lower()


def http(scope, url: str, path: str, *, method: str = "GET", body: bytes | None = None,
         operation_id: str | None = None, binary: bool = False, timeout: float = 120):
    for lease in scope.leases.values():
        lease.check()
    headers = {"Content-Type": "application/octet-stream" if body is not None else "application/json"}
    if operation_id:
        headers["X-Operation-ID"] = operation_id
    request = Request(url + path, data=body, headers=headers, method=method)
    try:
        with build_opener(NoRedirect()).open(request, timeout=timeout) as response:
            limit = MAX_BYTES if binary else 1024 * 1024
            data = response.read(limit + 1)
            if len(data) > limit:
                raise RuntimeError("Gateway response exceeds the supported size")
            declared = response.headers.get("Content-Length")
            if declared is not None and int(declared) != len(data):
                raise RuntimeError("Gateway response was truncated")
            return data if binary else json.loads(data)
    except HTTPError as error:
        details = error.read(8192).decode("utf-8", errors="replace")
        raise RuntimeError(f"Gateway HTTP {error.code}: {details}; no write was replayed") from error
    except (URLError, TimeoutError) as error:
        raise RuntimeError(f"Gateway request failed: {error}; inspect the operation ID before any retry") from error


def execute(args, tool_root: Path, scope, context) -> dict:
    from mosaico_cli.gateway import connected_devices, ensure_gateway, select_device, wait_for_data_link
    from mosaico_cli.project import resolve_project

    project = resolve_project(context.workspace, args.project, Path.cwd())
    cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(r"\bproject\(\s*CardenzaSynthMosaicoNative\s*\)", cmake):
        raise ValueError("native-files accepts only the CardenzaSynthMosaicoNative application project")
    args.project = str(project)
    payload = None
    local = args.local.expanduser().resolve() if args.action in {"put", "get"} else None
    if args.action == "put":
        if not local.is_file() or local.stat().st_size > MAX_BYTES:
            raise ValueError("Upload must be a regular file no larger than 32 MiB")
        with local.open("rb") as source:
            payload = source.read(MAX_BYTES + 1)
        if len(payload) > MAX_BYTES:
            raise ValueError("Upload grew beyond 32 MiB")
    elif args.action == "get" and local.exists() and not args.overwrite:
        raise ValueError("Download destination already exists; use --overwrite to replace it")

    session = ensure_gateway(context, None)
    device = select_device(connected_devices(context, session), args.device_id)
    wait_for_data_link(context, session, device, timeout=5)
    if len(session.connection_args) != 2 or session.connection_args[0] != "--url":
        raise RuntimeError("native-files requires the selected project's local Gateway")
    url = session.connection_args[1]
    parsed = urlsplit(url)
    if parsed.scheme != "http" or parsed.hostname not in {"127.0.0.1", "localhost", "::1"} or parsed.username:
        raise RuntimeError("Unexpected project Gateway URL")
    prefix = "/v2/devices/" + quote(args.device_id, safe="")
    before = identity(http(scope, url, prefix), args.device_id)
    volumes = http(scope, url, prefix + "/files/volumes")
    available = next((item for item in volumes.get("volumes", []) if item.get("id") == VOLUME), None)
    required = {"put": {"read", "write", "atomic_replace"}, "get": {"read"},
                "list": {"list"}, "delete": {"delete"}}[args.action]
    if available is None or not required.issubset(set(available.get("capability_names", []))):
        raise RuntimeError("Native application volume does not expose the required file capabilities")
    query = urlencode({"volume": VOLUME, "path": args.relative})
    operation_id = str(uuid.uuid4()) if args.action in {"put", "delete"} else None
    context.note(f"native-files {args.action} device={args.device_id} path={args.relative} operation_id={operation_id}")
    common = {"command": "native-files", "action": args.action, "device_id": args.device_id,
              "boot_id": before[1], "volume": VOLUME, "path": args.relative,
              "log": str(context.log_path)}
    if local is not None:
        common["local"] = str(local)
    if args.action == "list":
        result = http(scope, url, prefix + "/files?" + query + "&" + urlencode({"cursor": args.cursor, "limit": args.limit}))
        if identity(http(scope, url, prefix), args.device_id) != before:
            raise RuntimeError("Device boot/firmware changed during listing")
        return {**common, "ok": True, "listing": result}
    if operation_id:
        common["operation_id"] = operation_id
        # Preserve the identifier before transmission, including when the HTTP
        # connection fails after the device may already have committed bytes.
        (context.directory / "operation.json").write_text(json.dumps(common), encoding="utf-8")
        if args.action == "delete":
            # One explicit file only; no recursive deletion and no replay after an uncertain write.
            meta = http(scope, url, prefix + "/files/stat?" + query)
            if meta.get("kind") != "file":
                raise ValueError("Deletion requires an existing regular file")
            response = http(scope, url, prefix + "/file?" + query,
                            method="DELETE", operation_id=operation_id)
            result = response.get("file", {})
            if result.get("deleted") is not True:
                raise RuntimeError(f"Deletion was not confirmed; inspect operation {operation_id}")
            if identity(http(scope, url, prefix), args.device_id) != before:
                raise RuntimeError(f"Device boot/firmware changed during deletion; inspect operation {operation_id}")
            return {**common, "ok": True, "file": result}
        response = http(scope, url, prefix + "/file?" + query + "&overwrite=" + str(args.overwrite).lower(),
                        method="PUT", body=payload, operation_id=operation_id, timeout=300)
        result = response.get("file", {})
        digest = hashlib.sha256(payload).hexdigest()
        if result.get("size") != len(payload) or result.get("sha256") != digest:
            raise RuntimeError(f"Upload commit size/hash did not verify; inspect operation {operation_id}")
        meta = http(scope, url, prefix + "/files/stat?" + query)
        if meta.get("kind") != "file" or meta.get("size") != len(payload) or meta.get("etag") != result.get("etag"):
            raise RuntimeError(f"Upload metadata changed; inspect operation {operation_id}")
        if identity(http(scope, url, prefix), args.device_id) != before:
            raise RuntimeError(f"Device boot/firmware changed during upload; inspect operation {operation_id}")
        return {**common, "ok": True, "bytes": len(payload), "sha256": digest, "file": result}

    meta = http(scope, url, prefix + "/files/stat?" + query)
    if meta.get("kind") != "file" or not 0 <= int(meta.get("size", -1)) <= MAX_BYTES:
        raise ValueError("Download must be a regular file no larger than 32 MiB")
    data = http(scope, url, prefix + "/file?" + query, binary=True, timeout=300)
    after_meta = http(scope, url, prefix + "/files/stat?" + query)
    if len(data) != int(meta["size"]) or after_meta.get("etag") != meta.get("etag") or after_meta.get("size") != len(data):
        raise RuntimeError("Remote file changed during download; no local file was replaced")
    if identity(http(scope, url, prefix), args.device_id) != before:
        raise RuntimeError("Device boot/firmware changed during download; no local file was replaced")
    local.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=local.parent, prefix=local.name + ".", suffix=".tmp", delete=False) as destination:
            temporary = Path(destination.name)
            destination.write(data)
            destination.flush()
        if args.overwrite:
            temporary.replace(local)
        else:
            # An atomic same-directory hard link also refuses a destination
            # that appeared after preflight, without exposing partial bytes.
            os.link(temporary, local)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {**common, "ok": True, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "file": after_meta}


def main(argv=None, *, tool_root: Path) -> int:
    parser = argparse.ArgumentParser(prog="mosaico.py native-files", description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    for action in ("put", "get", "list", "delete"):
        command = commands.add_parser(action)
        if action == "put":
            command.add_argument("local", type=Path)
            command.add_argument("relative", type=relative_path)
        elif action == "get":
            command.add_argument("relative", type=relative_path)
            command.add_argument("local", type=Path)
        else:
            command.add_argument("relative", type=relative_path)
        if action == "list":
            command.add_argument("--cursor", type=int, default=0)
            command.add_argument("--limit", type=int, choices=range(1, 201), default=100, metavar="1..200")
        command.add_argument("--project", default="Devices/MosaicoNative")
        command.add_argument("--device-id", required=True)
        if action in {"put", "get"}:
            command.add_argument("--overwrite", action="store_true")
        command.add_argument("--json", action="store_true", help="Accepted for consistency; output is always JSON")
    args = parser.parse_args(argv)
    from mosaico_cli.errors import MosaicoError
    from mosaico_cli.runtime import RunContext
    from mosaico_cli.session_runtime import CURRENT_SCOPE, SessionScope
    from mosaico_cli.workspace import load_workspace
    scope = SessionScope()
    args.command = "native-files"
    args.public_command = "native-files " + args.action
    args.json = True
    scope.arguments = args
    token = CURRENT_SCOPE.set(scope)
    context = None
    try:
        workspace = replace(load_workspace(tool_root), gateway_source_policy="exact")
        context = RunContext(workspace, "native-files-" + args.action, json_output=True)
        result = execute(args, tool_root, scope, context)
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (MosaicoError, OSError, RuntimeError, ValueError) as error:
        print(json.dumps({"ok": False, "command": "native-files", "error": str(error),
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
