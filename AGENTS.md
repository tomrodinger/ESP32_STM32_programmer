# Agent Instructions

Refer to `README.md` for project details and architecture.

## Critical Workflow Rules

1.  **Verify Before Completion**: You MUST compile and run/test the code before declaring the task complete. Do not assume code works just because it looks correct. Use `pio run` to verify compilation.
2.  **Mandatory automated tests**: When you change code or add a feature, you MUST run the automated test suite before declaring completion. For this repo that means running `./test.sh` (in addition to `pio run` where relevant). Treat any non-zero exit code, errors, or warnings as a failure that must be fixed.
3.  **HARD STOP: Manual test plan + explicit human confirmation required for HW/UI changes**
    - If you change **hardware behavior** or the **web UI** (including any HTML, routes, buttons, endpoints, or anything under [`src/wifi_web_ui.cpp`](src/wifi_web_ui.cpp:1)), you MUST:
      1) Provide a short, numbered **manual test plan** (steps a human can execute).
      2) Explicitly ask the human to **run the plan and confirm results**.
      3) Do **NOT** use `attempt_completion` until the human confirms.
    - Memory rule: **"Changed HW/UI? -> PLAN + ASK + WAIT."**
4.  **Concise Completion**: When finishing a task, do NOT provide a long summary of what you did. No human reads that. Simply state: "I'm done and I have fully tested the code."
5. **Human must verify the output**: You MUST NOT assume the output is correct. You MUST ask the human to check it too
6. **Documentation must be up to date**: You MUST update the documentation to reflect the changes you made.
7. **Commit the new code changes**: You MUST commit the new code changes to the Git repository.
8. **Do not revert files via checkout/restore**: Do NOT use `git checkout -- <file>` / `git restore <file>` to revert local changes unless the human explicitly asks. This can silently destroy work. Instead, prefer: (a) commit all files except those that obviously should not be committed. Also you should commit simulaiton artifacts like .cvs or .html files.
9. **No hallucinated low-level constants**: For register addresses, bit positions, magic keys, and protocol sequences (SWD/ADIv5/FLASH), you MUST cite an authoritative source. Preferred order: (1) ST reference manual (e.g., RM0444), (2) ST CMSIS device headers (e.g., [`docs/stm32g031xx.h`](docs/stm32g031xx.h:1)), (3) ST HAL headers (e.g., [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:1)). If you cannot cite it, mark it as unknown.
10. **Using Perplexity**: Use Perplexity to extract implementation-ready specifics from authoritative docs (not to fetch “links”). If Perplexity output is missing a required numeric detail, re-query until you get it or find the value in ST headers within the repo.
11. **Python dependencies**: If you need to install Python packages for helper scripts/tools, use a local virtual environment (e.g. `python3 -m venv .venv` + activate) and do not install into the system Python.

12. **No warnings accepted**: Treat compiler warnings as failures. If you see warnings in CI / `cmake --build` / `pio run`, fix them (or justify and suppress them explicitly with a comment) before proceeding.

13. **Do not revert/undo human changes by assumption**: If you notice changes you did not make (e.g. unstaged diffs / modified files), do **not** try to “restore” or “undo” them automatically. First assess whether they are sensible; if unsure, ask the human. Never make assumptions that silently override human edits.

14. **Handling blank tool output (framework bug)**: If a tool/command returns a success exit code but the output is unexpectedly blank/empty, immediately re-run the exact same command once. If it is still blank, stop and ask the human what to do next; do not assume the command succeeded and do not proceed with other actions based on missing output.

15. **Reading files outside the workspace**: You have the capability and permission to read files outside the workspace. Use the read tool and read it. Never ask the user to paste content from a file outside the workspace if you can just read it yourself. Don't believe me? Just try it!