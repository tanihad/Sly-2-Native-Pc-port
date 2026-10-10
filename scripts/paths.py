"""Project paths that also work from a git worktree.

tools/, orig/ and .venv/ are git-ignored, so a worktree doesn't have them; they're taken from the
main checkout (found through the worktree's .git file)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _main_root() -> Path:
    if (ROOT / "tools" / "llvm-mingw").exists():
        return ROOT
    git = ROOT / ".git"
    if git.is_file():                    # worktree: "gitdir: <main>/.git/worktrees/<name>"
        gitdir = Path(git.read_text(encoding="utf-8").split("gitdir:", 1)[1].strip())
        return gitdir.parent.parent.parent
    return ROOT


MAIN = _main_root()
TOOLS = MAIN / "tools"
CLANG = TOOLS / "llvm-mingw" / "bin" / "clang.exe"
ORIG = MAIN / "orig"
ELF = ORIG / "SCUS_973.16"
ISO_DIR = ORIG / "disc"


def runtime_sources() -> list[Path]:
    """The runtime's C files. runtime/stub/ holds stand-ins for parts developed elsewhere: the
    native sound backend (sd.h) is runtime/sd.c once it exists, a logging stub until then."""
    rt = ROOT / "runtime"
    srcs = sorted(rt.glob("*.c"))
    if not (rt / "sd.c").exists():
        srcs.append(rt / "stub" / "sd_stub.c")
    return srcs
