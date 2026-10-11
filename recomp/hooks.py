"""Entry hooks: the few functions whose calls a runtime debug tool must see in every build.

Debug builds trace every function entry (PS2_TRACE -> ps2_calls_record when a tool asks for it);
release builds compile tracing out. A function listed here also gets `PS2_HOOK(<addr>)` at its
entry in every build: a test of `ps2_hooks_on`, which stays 0 unless a debug tool switches it on,
then `ps2_hook(ctx, addr)` (runtime/ps2_runtime.c), which hands the call to that tool. In release
builds ps2_hook also counts the PS2_INPUT function anchors (`@<addr>[/<ra>]`), so anchors on these
functions work there too (func_table.c lists them in ps2_hook_addrs). Nothing in the game changes;
the tools are off by default. Like the native overrides, an entry is guarded by a hash of the
function's original bytes, so another executable gets no hook.
See notes/recompiler-design.md ("Entry hooks").
"""
from dataclasses import dataclass

from .overrides import function_sha1


@dataclass(frozen=True)
class Hook:
    name: str
    sha1: str          # of the function's original bytes [start, end)
    reason: str


HOOKS: dict[int, Hook] = {
    0x00187D80: Hook(
        "pad_button_pressed", "9179a5643e7e6577f207e8855be40e349eb51ec7",
        "tests a pad's buttons (pad, mask); the title screen (0x1DD920) asks it for START from the "
        "call returning to 0x1DD96C once it accepts START: the input anchor @187D80/1DD96C that "
        "scripts (warp_run.ps1) use to press START on the title, in release builds too"),
    0x0022B490: Hook(
        "level_transition", "9191080cf69aee0a7881377bfb31eb6c706a54bf",
        "queues a level load (world, descriptor, start point, ?, flags); PS2_WARP_TASK "
        "(runtime/warp.c) turns the title screen's Resume Game call into a job start"),
}


def hook_for(prog, start: int, end: int) -> Hook | None:
    """The entry hook for the function at start, if its bytes are the expected ones."""
    h = HOOKS.get(start)
    if h and function_sha1(prog, start, end) == h.sha1:
        return h
    return None
