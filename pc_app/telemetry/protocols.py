"""
Which wire protocol the ground station speaks: v1 or v2.

Both proto.py and proto_v2.py expose the same handful of names the rest of
the app touches - StreamParser, decode_frame, Frame, NodeRecord, CHANNELS,
NODE_LABELS, NODE_COUNT, FRAME_SIZE, FRAME_PERIOD_MS, the NF_* flags - so
everything downstream can work against whichever is selected without caring
which one it got. That is the whole reason this module is one variable and
two functions rather than an abstraction layer.

Selection happens exactly once, in run.py, before any source is constructed.
Switching mid-run is not supported and would leave a half-decoded stream, so
there is deliberately no way to do it.

v1 is the default because it is what the hardware currently speaks and what
is proven working end to end. v2 is opt-in via `--proto v2` until the v2
firmware exists - see PROJECT_STATUS.md.
"""

from __future__ import annotations

from types import ModuleType

from . import proto as _v1
from . import proto_v2 as _v2

V1 = "v1"
V2 = "v2"
CHOICES = (V1, V2)

_active: ModuleType = _v1
_active_name: str = V1


def use(version: str) -> ModuleType:
    """
    Select the protocol. Call once at startup, before building a source.
    Returns the selected module so a caller can log what it got.
    """
    global _active, _active_name

    if version not in CHOICES:
        raise ValueError(f"unknown protocol {version!r}, expected one of {CHOICES}")

    _active = _v2 if version == V2 else _v1
    _active_name = version
    return _active


def active() -> ModuleType:
    """
    The currently selected protocol module.

    Call this at use time rather than binding the result at import time -
    `from .protocols import active` then `active().NODE_COUNT` picks up the
    selection, whereas `p = active()` at module scope would capture v1 before
    run.py ever gets a chance to switch it.
    """
    return _active


def active_name() -> str:
    return _active_name
