# Checkpoint protocol for agents (resume after interruption)

Any agent or session can be cut off at any moment (usage limits, crashes). Unsaved reasoning is
lost; files and commits are not. So every agent working on this project keeps its progress on
disk, and every agent starts by looking for a previous attempt.

## At start

1. Look for an earlier attempt at your task before doing anything:
   - `notes/wip/<task>.md` in your working tree (your checkpoint file),
   - `git log --oneline main..HEAD` and `git status` on your branch/worktree.
2. If either exists, read them, then **continue from the "Next" list**. Do not redo finished
   steps and do not rewrite working code from scratch; verify it builds/tests, then carry on.

## While working

After every meaningful step (a component compiles, a group of tests passes, an investigation
reaches a conclusion; at least every ~30 minutes of work):

1. Update `notes/wip/<task>.md`:
   - **Goal**: one line.
   - **Done**: finished steps, with file names.
   - **In progress**: what you are doing right now, and how far it got.
   - **Next**: the remaining steps, in order, concrete enough for someone else to pick up.
   - **Findings / traps**: facts learned the hard way (register layouts, failed approaches).
2. Commit it with the code: `git commit -m "wip(<task>): <step>"`. Small WIP commits are fine.

## At the end

Mark the checkpoint file **Status: complete**, keep the Findings section (it is useful history),
and make the final commit.
