#!/usr/bin/env python3
"""Delegate to the real helper, then fail after the selected target became visible."""

import os
import json
import subprocess
import sys
from typing import Optional


def _argument(name: str) -> Optional[str]:
    try:
        return sys.argv[sys.argv.index(name) + 1]
    except (ValueError, IndexError):
        return None


def _run_real(*, capture_output: bool = False) -> subprocess.CompletedProcess:
    lock_fd = _argument("--lock-fd")
    pass_fds = (int(lock_fd),) if lock_fd is not None else ()
    return subprocess.run(
        [sys.executable, os.environ["PROVISION_TEST_REAL_HELPER"], *sys.argv[1:]],
        check=False,
        capture_output=capture_output,
        text=capture_output,
        pass_fds=pass_fds,
    )


def _inject_external_update(snapshot: subprocess.CompletedProcess) -> int:
    counter_path = os.environ["PROVISION_TEST_SNAPSHOT_COUNTER"]
    try:
        with open(counter_path, encoding="ascii") as counter_file:
            count = int(counter_file.read().strip())
    except FileNotFoundError:
        count = 0
    count += 1
    with open(counter_path, "w", encoding="ascii") as counter_file:
        counter_file.write(f"{count}\n")

    if count == 1:
        path = _argument("--path") or ""
        temporary = f"{path}.noncooperative.{os.getpid()}"
        content = json.dumps(
            {
                "schema_version": 1,
                "topology": os.environ["PROVISION_TEST_EXTERNAL_TOPOLOGY"],
            },
            separators=(",", ":"),
        ).encode("ascii") + b"\n"
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            os.write(fd, content)
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(temporary, path)
    sys.stdout.write(snapshot.stdout)
    sys.stderr.write(snapshot.stderr)
    return snapshot.returncode


def main() -> int:
    if (
        len(sys.argv) > 1
        and sys.argv[1] == "snapshot"
        and _argument("--path") == os.environ.get("PROVISION_TEST_FAIL_PATH")
        and os.environ.get("PROVISION_TEST_EXTERNAL_TOPOLOGY")
    ):
        return _inject_external_update(_run_real(capture_output=True))

    result = _run_real()
    if result.returncode:
        return result.returncode
    if (
        len(sys.argv) > 1
        and sys.argv[1] == "set"
        and _argument("--path") == os.environ.get("PROVISION_TEST_FAIL_PATH")
        and not os.environ.get("PROVISION_TEST_EXTERNAL_TOPOLOGY")
    ):
        marker = os.environ.get("PROVISION_TEST_POST_SET_MARKER")
        if marker:
            with open(marker, "w", encoding="ascii"):
                pass
            subprocess.run(["sleep", "1"], check=True)
        return 74
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
