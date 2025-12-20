# Agent Instructions

Refer to `README.md` for project details and architecture.

## Critical Workflow Rules

1.  **Verify Before Completion**: You MUST compile and run/test the code before declaring the task complete. Do not assume code works just because it looks correct. Use `pio run` to verify compilation.
2.  **Concise Completion**: When finishing a task, do NOT provide a long summary of what you did. No human reads that. Simply state: "I'm done and I have fully tested the code."
3. **Human must verify the output**: You MUST NOT assume the output is correct. You MUST ask the human to check it too
4. **Documentation must be up to date**: You MUST update the documentation to reflect the changes you made.
5. **Commit the new code changes**: You MUST commit the new code changes to the Git repository.
6. **No hallucinated low-level constants**: For register addresses, bit positions, magic keys, and protocol sequences (SWD/ADIv5/FLASH), you MUST cite an authoritative source. Preferred order: (1) ST reference manual (e.g., RM0444), (2) ST CMSIS device headers (e.g., [`docs/stm32g031xx.h`](docs/stm32g031xx.h:1)), (3) ST HAL headers (e.g., [`docs/stm32g0xx_hal_flash.h`](docs/stm32g0xx_hal_flash.h:1)). If you cannot cite it, mark it as unknown.
7. **Using Perplexity**: Use Perplexity to extract implementation-ready specifics from authoritative docs (not to fetch “links”). If Perplexity output is missing a required numeric detail, re-query until you get it or find the value in ST headers within the repo.
