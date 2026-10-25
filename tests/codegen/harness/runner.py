"""Process runner for the differential harness.

One case owns one directory.  Both tools are invoked with an argv list (never
through a shell), from that directory, with a fixed environment and a timeout.
The runner captures the exit code, stdout, stderr, wall time and the files a
tool wrote below a watched directory (relative path, size, SHA-256).
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass, field


WINDOWS_CRASH_CODES = {
    0xC0000005: "access violation",
    0xC00000FD: "stack overflow",
    0xC0000409: "stack buffer overrun / fail-fast",
    0xC000013A: "control-c exit",
}


def repo_root(start: os.PathLike[str] | str) -> pathlib.Path:
    """Locate the repository root above a harness copy.

    The tracked harness lives at ``tests/codegen/harness`` and the CI copy at
    ``build/codegen-harness``, so the fixed ``parents[2]`` of the original
    ``build/verification/harness`` location is not portable.  Both layouts are
    accepted because both are inside a checkout that owns ``CMakeLists.txt``
    and ``tests/codegen``.
    """
    here = pathlib.Path(start).resolve()
    for candidate in (here, *here.parents):
        if ((candidate / "CMakeLists.txt").is_file()
                and (candidate / "tests/codegen").is_dir()):
            return candidate
    raise RuntimeError("cannot locate the repository root above %s" % here)


def resolve_tool(value: os.PathLike[str] | str) -> pathlib.Path:
    """Resolve a tool argument that may be an absolute path or a PATH name."""
    path = pathlib.Path(value)
    if path.is_file():
        return path
    found = shutil.which(str(value))
    return pathlib.Path(found) if found else path


@dataclass
class FileInfo:
    path: str
    size: int
    sha256: str

    def as_dict(self) -> dict:
        return {"path": self.path, "size": self.size, "sha256": self.sha256}


@dataclass
class ExecResult:
    argv: list[str]
    cwd: str
    rc: int | None
    timed_out: bool
    duration_s: float
    stdout: bytes
    stderr: bytes
    files: list[FileInfo] = field(default_factory=list)
    error: str | None = None

    @property
    def crashed(self) -> bool:
        if self.rc is None:
            return False
        # A POSIX shell reports a fatal signal as a negative return code.
        return self.rc in WINDOWS_CRASH_CODES or self.rc < 0

    @property
    def crash_name(self) -> str | None:
        if not self.crashed:
            return None
        if self.rc in WINDOWS_CRASH_CODES:
            return WINDOWS_CRASH_CODES[self.rc]
        try:
            return signal.Signals(-self.rc).name.lower()
        except ValueError:
            return "signal %d" % -self.rc

    def stdout_text(self, limit: int | None = 4000) -> str:
        return _text(self.stdout, limit)

    def stderr_text(self, limit: int | None = 4000) -> str:
        return _text(self.stderr, limit)

    def as_dict(self) -> dict:
        return {
            "argv": self.argv,
            "cwd": self.cwd,
            "rc": self.rc,
            "timed_out": self.timed_out,
            "crashed": self.crashed,
            "crash": self.crash_name,
            "duration_s": round(self.duration_s, 4),
            "stdout": self.stdout_text(),
            "stderr": self.stderr_text(),
            "files": [item.as_dict() for item in self.files],
            "error": self.error,
        }


def _text(data: bytes, limit: int | None) -> str:
    text = data.decode("utf-8", "replace")
    if limit is not None and len(text) > limit:
        return text[:limit] + "\n... [truncated]"
    return text


def sha256_file(path: os.PathLike[str] | str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_tree(root: os.PathLike[str] | str) -> list[FileInfo]:
    """Snapshot every regular file below *root*, sorted by relative path."""
    root = os.path.abspath(root)
    result: list[FileInfo] = []
    if not os.path.isdir(root):
        return result
    for directory, _, names in os.walk(root):
        for name in names:
            full = os.path.join(directory, name)
            if not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, root).replace("\\", "/")
            result.append(FileInfo(rel, os.path.getsize(full), sha256_file(full)))
    result.sort(key=lambda item: item.path)
    return result


def build_environment(case_dir: str,
                      overrides: dict[str, str] | None = None) -> dict[str, str]:
    """A fixed environment: locale, time zone and per-case temp variables."""
    env = dict(os.environ)
    temp = os.path.join(case_dir, "tmp")
    os.makedirs(temp, exist_ok=True)
    env.update({
        "LC_ALL": "C",
        "LANG": "C",
        "TZ": "UTC",
        "TMP": temp,
        "TEMP": temp,
        "TMPDIR": temp,
    })
    if overrides:
        env.update(overrides)
    return env


def execute(
    argv: list[str],
    cwd: str,
    timeout_s: float,
    watch_dir: str | None = None,
    env_overrides: dict[str, str] | None = None,
) -> ExecResult:
    """Run one argv list without a shell and capture everything it produces."""
    cwd = os.path.abspath(cwd)
    os.makedirs(cwd, exist_ok=True)
    env = build_environment(cwd, env_overrides)
    started = time.monotonic()
    try:
        completed = subprocess.run(
            argv,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
            shell=False,
        )
        rc = completed.returncode
        stdout = completed.stdout or b""
        stderr = completed.stderr or b""
        timed_out = False
        error = None
    except subprocess.TimeoutExpired as expired:
        rc = None
        stdout = expired.stdout or b""
        stderr = expired.stderr or b""
        timed_out = True
        error = "timeout after %.1fs" % timeout_s
    except OSError as os_error:
        rc = None
        stdout = b""
        stderr = b""
        timed_out = False
        error = "failed to start: %s" % os_error
    duration = time.monotonic() - started
    watched = watch_dir if watch_dir is not None else cwd
    return ExecResult(
        argv=list(argv),
        cwd=cwd,
        rc=rc,
        timed_out=timed_out,
        duration_s=duration,
        stdout=stdout,
        stderr=stderr,
        files=file_tree(watched),
        error=error,
    )


def write_capture(case_dir: str, label: str, result: ExecResult) -> None:
    """Persist the raw streams next to the case, even when the run succeeds."""
    with open(os.path.join(case_dir, label + ".stdout.bin"), "wb") as handle:
        handle.write(result.stdout)
    with open(os.path.join(case_dir, label + ".stderr.bin"), "wb") as handle:
        handle.write(result.stderr)
