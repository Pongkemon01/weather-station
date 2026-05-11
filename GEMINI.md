# GEMINI.md

Project context for AI coding agents.

Read `CLAUDE.md` — it is the single source of truth for project identity, directory layout, peripheral allocation, FreeRTOS task map, mutex rules, coding standards, "must never do" rules, and per-task reading list.

Companion docs:

- `OTA_Firmware_Architecture.md` — as-built firmware OTA architecture (FRAM layout, state machine, bootloader, A7670 modem flow)
- `Server_Architecture.md` — as-built FastAPI server (mTLS, OTA endpoints, slot algorithm, schema)
- `Server_Implementation_Plan.md`, `User_Management_Implementation_Plan.md` — phase summaries
- `Server_Test_Plan.md`, `User_Management_Test_Plan.md` — black-box verifier coverage
- `IMPLEMENTATION_STATUS.md` — current phase status (one row per phase)
- `https_manual.md`, `ntp_manual.md` — A7670E AT command reference (vendor)

Before exploring the codebase, prefer the project's knowledge graph (`code-review-graph` MCP and `graphify-out/`) over `grep`/`glob`. See `CLAUDE.md` "MCP Tools" section.
