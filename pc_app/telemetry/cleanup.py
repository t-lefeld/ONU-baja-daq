"""
Deletes old files out of paths.logs_dir() - the timestamped CSV recordings and
the rolling telemetry.log, not the SD-card captures under field_data/.

Why this is its own module instead of a couple of lines in run.py: deleting
things is the one operation in this app where a bug does not show up as a
stack trace, it shows up as "my log from last weekend's test is gone." That
earns it a real API with its own tests instead of inline argparse-handler
logic, and a safety guard that does not trust the caller to have gotten the
glob right.

What counts as a "log" here (see LOG_PATTERNS below) is deliberately narrow:

    telemetry_*.csv   - CsvRecorder output, named by run.timestamped("telemetry", ".csv")
    telemetry*.log    - the FileHandler target from run.setup_logging(), currently
                         the single fixed name "telemetry.log", but the trailing
                         "*" tolerates a future rotated name (telemetry.log.1 etc.)
                         without a second code change.

Deliberately NOT included: raw_*.tlm raw-stream captures. Those are the closest
thing this app writes to a primary data source rather than a byproduct - the
same category as field_data/ - so pruning them is left to the operator doing
it by hand rather than a --clean-logs sweep. If that turns out to be wrong, add
"raw_*.tlm" to LOG_PATTERNS; nothing else in this module assumes the pattern
list has exactly two entries.

field_data/ is never in play here at all - list_logs() and prune_logs() only
ever look inside logs_dir(), and _is_safe_target() below refuses to act on
anything that is not inside the resolved logs_dir, as a second line of defense
in case a future caller passes a path that has wandered outside it.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from . import paths

log = logging.getLogger("telemetry")

# Glob patterns (Path.glob syntax, matched against filenames directly inside
# logs_dir - no recursion) for what this module is willing to delete. Keep
# this narrow: the cost of a pattern that is too loose is someone's telemetry
# silently disappearing, the cost of one that is too tight is a stale file
# that has to be deleted by hand once. Err tight.
LOG_PATTERNS: tuple[str, ...] = (
    "telemetry_*.csv",
    "telemetry*.log",
)


@dataclass(frozen=True)
class LogFile:
    """One file in logs_dir, with the stat() fields prune_logs needs to decide."""

    path: Path
    bytes: int
    modified: float  # st_mtime, epoch seconds
    age_days: float


@dataclass(frozen=True)
class PruneResult:
    """
    What a prune_logs() call did (or, for dry_run=True, would have done).

    errors is a list of "<filename>: <reason>" strings rather than exceptions
    or a dict keyed by path, because the only thing anything downstream of
    this module does with it is print it - a plain string is the least there
    is to get wrong turning it into a CLI message.
    """

    deleted: list[LogFile]
    kept: list[LogFile]
    freed_bytes: int
    dry_run: bool
    errors: list[str] = field(default_factory=list)


def _matches_log_pattern(name: str) -> bool:
    return any(Path(name).match(pattern) for pattern in LOG_PATTERNS)


def _is_safe_target(path: Path, resolved_logs_dir: Path) -> bool:
    """
    Last line of defense before an unlink(): the file must resolve to
    somewhere directly inside logs_dir, not a symlink pointing elsewhere and
    not a subdirectory (field_data/ lives three levels up from here, but a
    future bug that hands this function the wrong root should still not be
    able to reach it).

    Symlinks are refused outright rather than resolved-and-checked: a symlink
    sitting in logs_dir named telemetry_foo.csv that points at something
    important elsewhere is exactly the kind of file this guard exists for,
    and the safe answer is "never delete through a link", not "delete the
    link's target if it happens to also be under logs_dir".
    """
    if path.is_symlink():
        return False
    if not path.is_file():
        return False
    try:
        resolved = path.resolve(strict=True)
    except OSError:
        return False
    return resolved.parent == resolved_logs_dir


def list_logs(logs_dir: Path | None = None) -> list[LogFile]:
    """
    Inventory of deletable-category files directly in logs_dir, newest first.

    Newest-first because both call sites (the --list-logs printout and
    prune_logs's keep=N) want "the N most recent" to be a prefix slice rather
    than something they each have to sort themselves.
    """
    root = logs_dir if logs_dir is not None else paths.logs_dir()
    root = root.resolve()

    now = time.time()
    out: list[LogFile] = []

    if not root.is_dir():
        return out

    for entry in root.iterdir():
        if entry.is_symlink() or not entry.is_file():
            continue
        if not _matches_log_pattern(entry.name):
            continue
        try:
            st = entry.stat()
        except OSError as exc:
            log.debug("Skipping %s in logs inventory (%s)", entry, exc)
            continue
        age_days = max(0.0, (now - st.st_mtime) / 86400.0)
        out.append(LogFile(path=entry, bytes=st.st_size, modified=st.st_mtime,
                            age_days=age_days))

    out.sort(key=lambda f: f.modified, reverse=True)
    return out


def prune_logs(
    logs_dir: Path | None = None,
    *,
    keep: int | None = None,
    older_than_days: float | None = None,
    dry_run: bool = False,
    protect: Iterable[Path] = (),
) -> PruneResult:
    """
    Delete old logs_dir files, subject to keep / older_than_days / protect.

    keep and older_than_days can be combined (delete what falls outside
    "one of the N newest" AND is older than the cutoff would be redundant, so
    combining them here means "delete anything older than older_than_days,
    but never fewer than `keep` survive it" - i.e. keep is always honoured as
    a floor on how many files remain, even if older_than_days alone would have
    deleted more than that).

    If both are None, nothing is deleted - see the module-level warning in
    the CLI help text this backs. A bare `prune_logs()` call is far more
    likely to be a caller forgetting to pass a filter than a caller who
    actually wants every log gone, so silence is the wrong default and
    "delete nothing" is the safe one.
    """
    root = logs_dir if logs_dir is not None else paths.logs_dir()
    root = root.resolve()
    protected = {p.resolve() for p in protect if p is not None}

    files = list_logs(root)

    result_deleted: list[LogFile] = []
    result_kept: list[LogFile] = []
    errors: list[str] = []
    freed = 0

    if keep is None and older_than_days is None:
        # No filter given: everything is "kept", nothing is touched.
        return PruneResult(deleted=[], kept=files, freed_bytes=0, dry_run=dry_run,
                            errors=[])

    for index, lf in enumerate(files):
        # keep is a FLOOR, not just one of two independent delete triggers:
        # if a file is among the `keep` newest it survives no matter how old
        # it is. Doing this the other way round (OR the two conditions) means
        # `--keep 10 --older-than 30` on a rig that sat unused for a month
        # deletes every log you have, which is the opposite of what someone
        # asking to keep ten of them meant.
        if keep is not None and index < keep:
            result_kept.append(lf)
            continue

        should_delete = False
        if keep is not None and index >= keep:
            should_delete = True
        if older_than_days is not None and lf.age_days > older_than_days:
            should_delete = True

        if lf.path.resolve() in protected:
            should_delete = False

        if not should_delete:
            result_kept.append(lf)
            continue

        if not _is_safe_target(lf.path, root):
            # Should not happen - list_logs() only yields files already
            # inside root - but a file could be replaced by a symlink or
            # moved between the listing and the delete, and re-checking here
            # costs nothing next to the alternative of unlinking blind.
            errors.append(f"{lf.path.name}: no longer a safe target, skipped")
            result_kept.append(lf)
            continue

        if dry_run:
            result_deleted.append(lf)
            freed += lf.bytes
            continue

        try:
            lf.path.unlink()
            result_deleted.append(lf)
            freed += lf.bytes
        except OSError as exc:
            # PermissionError (Windows file lock on the log currently being
            # written, antivirus scanning it, etc.) must not abort the run -
            # one stuck file should not stop the other nine from being freed.
            errors.append(f"{lf.path.name}: {exc}")
            result_kept.append(lf)

    return PruneResult(
        deleted=result_deleted,
        kept=result_kept,
        freed_bytes=freed,
        dry_run=dry_run,
        errors=errors,
    )
