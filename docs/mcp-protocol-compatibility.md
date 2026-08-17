# MCP Protocol Compatibility Matrix

This document records implemented qtLLM MCP Host behavior. It describes the
current code, not planned support. Planned work is tracked in
[the MCP Host improvement plan](mcp-host-improvement-plan.md).

## Compatibility Policy

- qtLLM advertises the newest protocol version listed as supported below.
- A Server must return a version in the supported set during `initialize`.
- Unknown Server capabilities are preserved as JSON but are not implicitly
  enabled or trusted.
- A protocol feature is usable only when both the negotiated version and the
  Server capability permit it.
- Server annotations, descriptions, instructions, Prompts, Resources, and
  results are untrusted input. Local policy remains authoritative.

## Protocol Versions

| Version | Status | Transport | Conformance coverage |
| --- | --- | --- | --- |
| `2024-11-05` | Supported | stdio | initialize, initialized, basic Tools, errors, timeout, local cancellation |
| Any other version | Rejected | none | unsupported-version integration test |

Additional versions must not be added to this table until their lifecycle and
feature differences have dedicated Fake Server coverage.

## Host Features

| Area | Feature | Current status |
| --- | --- | --- |
| Lifecycle | `initialize` and `notifications/initialized` | Supported |
| Lifecycle | Validate negotiated protocol version | Supported |
| Lifecycle | Preserve Server capabilities, info, and instructions | Supported internally |
| Lifecycle | Multi-state Server health model | Not implemented |
| Transport | stdio JSON-RPC | Supported |
| Transport | Streamable HTTP | Not implemented |
| Tools | `tools/list`, single page | Supported |
| Tools | Cursor pagination | Not implemented |
| Tools | `tools/call` | Supported |
| Tools | Input schema validation | Supported subset |
| Tools | Output schema and annotations | Not implemented |
| Tools | List-changed notification | Not implemented |
| Results | Text content and raw result preservation | Supported |
| Results | General content-block model | Not implemented |
| Results | `structuredContent` object | Supported |
| Results | `structuredContent` array or scalar | Not implemented |
| Cancellation | Stop local wait and ignore late response | Supported |
| Cancellation | Send protocol cancellation notification | Not implemented |
| Notifications | Parse transport notifications | Supported at transport boundary |
| Notifications | Progress and logging routing | Not implemented |
| Resources | Discovery, read, templates, subscriptions | Not implemented |
| Prompts | Discovery and get | Not implemented |
| Roots | Per-Server authorized roots | Not implemented |
| Server requests | Sampling and user input requests | Not implemented |

## Current Safety Boundaries

- MCP Servers run outside the model worker. stdio Servers are child processes
  of the Qt application.
- stdout is JSON-RPC only; stderr is diagnostic output.
- Tool names are qualified as `serverId.toolName`.
- Tools must pass the configured allowlist, local JSON schema validation, and
  `ToolPolicy` before execution.
- Unknown third-party tools are not classified as safe from their names or
  Server-provided annotations.
- The bundled filesystem Server resolves canonical paths beneath explicitly
  supplied roots and remains a reference Server, not an in-process Host tool.

## Stage 0 Regression Coverage

- A Server returning an unsupported protocol version never reaches initialized
  state and receives no `notifications/initialized` notification.
- A Server that does not declare the Tools capability cannot list or call
  tools, even if it implements those methods.
- Tool catalog output is deterministically ordered and always valid JSON.
- A size budget omits complete tool definitions; it never truncates a name,
  description, schema, UTF-8 sequence, or closing JSON delimiter.
- Tests run with the Fake MCP Server and do not require a model or network.
