# TODO

## Persist conversations across restarts

Status: deferred from the current chat MVP.

Current behavior:

- Multi-turn context is kept only while `qtLLM` is running.
- Closing the application clears all conversation messages.
- Only the last successfully loaded model path is persisted in `qtLLM.ini`.

Future work:

- Store conversations and messages in SQLite.
- Restore conversation history after an application restart.
- Add create, rename, delete, and select conversation workflows.
- Persist only completed assistant answers; do not store hidden reasoning as
  conversation context.
- Keep database migrations atomic and preserve user data during upgrades.
- Add tests for restart recovery, interrupted generation, deletion, and schema
  migration.
