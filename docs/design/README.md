# Design documents

High-level designs for the Lindbergh cores: what each part is for, how it
is built and why, how it is tested and what is still open. They describe the
design as it is and are updated in the same pull request as a change that
moves it. The dated history is in HANDOFF.md and the current state in
STATE.md. Designs shared with other cores (the x86 recompiler, mtlgles) are
in Arcade-Foundry-Cores-Common's `docs/design/`; the app's own are in
Arcade Foundry: Insert Coin's `docs/design/`.

They are public: they go out with each release and pass its scan, so they
name no people, machines or private paths and hold nothing taken from ROMs.
Working notes that shouldn't ship go in a `*-DEVELOPER.md` file.

| Document | Covers | State |
|---|---|---|
| [LINDBERGH.md](LINDBERGH.md) | The Lindbergh cores: the plan, the runtime, graphics per platform, frames and time, the cabinet, testing | HotD4 and OutRun 2 SP SDX released; Let's Go Jungle! and Race TV for 1.2.0; Metal default on Apple platforms next |

Each document follows the same outline as the app's: Purpose and scope,
Requirements, Design, Data and settings, Platforms, Testing, Open items,
Decisions.
