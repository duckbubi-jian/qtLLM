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
| `2025-06-18` | Supported | stdio | lifecycle, bidirectional ping, Tools, Resources, Prompts, Roots, rich results |
| `2025-03-26` | Supported | stdio | initialize parsing and shared stdio/core-message compatibility |
| `2024-11-05` | Supported | stdio | lifecycle, paginated and dynamic Tools, rich results, errors, timeout, protocol cancellation |
| Any other version | Rejected | none | unsupported-version integration test |

Additional versions must not be added to this table until their lifecycle and
feature differences have dedicated Fake Server coverage.

## Host Features

| Area | Feature | Current status |
| --- | --- | --- |
| Lifecycle | `initialize` and `notifications/initialized` | Supported |
| Lifecycle | Validate negotiated protocol version | Supported |
| Lifecycle | Preserve Server capabilities, info, and instructions | Supported |
| Lifecycle | Multi-state Server health model | Supported |
| Lifecycle | Start, stop, restart, enable, and disable per Server | Supported |
| Transport | stdio JSON-RPC | Supported |
| Transport | Streamable HTTP | Intentionally out of scope |
| Utilities | Host-to-Server and Server-to-Host `ping` | Supported |
| Tools | `tools/list` | Supported |
| Tools | Cursor pagination and atomic snapshots | Supported |
| Tools | `tools/call` | Supported |
| Tools | Input schema validation | Supported subset including local `$ref`, `const`, type unions, `allOf`, `anyOf`, and `oneOf` |
| Tools | Output schema and annotations | Supported |
| Tools | List-changed notification and refresh coalescing | Supported |
| Results | Text content and raw result preservation | Supported |
| Results | General content-block preservation | Supported |
| Results | Unknown content-block type reporting | Supported |
| Results | `structuredContent` object, array, or scalar | Supported |
| Cancellation | Stop local wait and ignore late response | Supported |
| Cancellation | Send protocol cancellation notification | Supported |
| Notifications | Parse transport notifications | Supported at transport boundary |
| Notifications | Typed progress and logging routing | Supported, rate limited per Server |
| Diagnostics | Per-Server state, stderr, logging, and error history | Supported, bounded and redacted |
| Host UI | Server overview, Tools policy, and lifecycle controls | Supported |
| Resources | Discovery, read, templates, subscriptions | Supported |
| Resources | `notifications/resources/list_changed` and `notifications/resources/updated` | Supported, subscribed URIs only |
| Prompts | Discovery and get | Supported |
| Prompts | `notifications/prompts/list_changed` | Supported |
| Completion | `completion/complete` for Prompt and Resource template references | Supported, explicit Host control-panel entry |
| Logging | `logging/setLevel` | Supported, validated per Server and persisted after success |
| Instructions | Per-Server opt-in boundary and bounded Agent context | Supported, default enabled for backward compatibility |
| Roots | Per-Server authorized roots | Supported |
| Server requests | `roots/list` | Supported, explicit configured roots only |
| Server requests | Unknown or unsupported methods | Rejected with standard JSON-RPC error |
| Server requests | Sampling and user input requests | Not implemented |

## Current Safety Boundaries

- MCP Servers run outside the model worker. stdio Servers are child processes
  of the Qt application.
- The Host does not implement HTTP transport or HTTP authentication. Networked
  MCP Servers and web-search integration are currently deferred.
- stdout is JSON-RPC only; stderr is diagnostic output.
- The Host UI retains at most 200 diagnostic entries per Server. Credential
  fields, common secret assignments, and Bearer tokens are redacted before
  display or logging, and individual entries are length limited.
- Tool names are qualified as `serverId.toolName`.
- Tools must pass the configured allowlist, local JSON schema validation, and
  `ToolPolicy` before execution.
- Unknown third-party tools are not classified as safe from their names or
  Server-provided annotations.
- The bundled filesystem Server resolves canonical paths beneath explicitly
  supplied roots and remains a reference Server, not an in-process Host tool.

## Regression Coverage

- A Server returning an unsupported protocol version never reaches initialized
  state and receives no `notifications/initialized` notification.
- A Server that does not declare the Tools capability cannot list or call
  tools, even if it implements those methods.
- Tool catalog output is deterministically ordered and always valid JSON.
- A size budget omits complete tool definitions; it never truncates a name,
  description, schema, UTF-8 sequence, or closing JSON delimiter.
- The Agent prompt keeps a separately budgeted compact tool index when complete
  definitions are omitted. The index resolves local references and preserves
  required fields, discriminator values, and union call shapes.
- Local input and output validation follows local `$ref` values and validates
  `const`, string or array `type`, `allOf`, `anyOf`, and `oneOf` before a tool
  call is sent or a structured result is accepted.
- Agent input-validation failures preserve the qualified tool name, instance
  path, schema path, validation keyword, and readable message. The focused
  correction prompt receives that diagnostic together with the rejected
  arguments and the failed tool's bounded contract.
- Tests run with the Fake MCP Server and do not require a model or network.
- Two Servers can expose same-named tools independently; failure or restart of
  one Server does not revoke the other Server's capabilities.
- A `Failed` Server whose transport process is still running is stopped before
  restart, so the runtime cannot remain stuck in `Starting` after a transport
  start no-op.
- The Host advertises `2025-06-18`, accepts the three explicitly listed
  protocol versions, and rejects untested versions.
- Ping has fake-transport and real-stdio coverage in both directions.
- Multi-page tool discovery remains invisible until every page succeeds. A
  later-page failure or repeated cursor retains the previous snapshot.
- Resource, resource-template, and Prompt catalogs use the same atomic
  pagination rules. A failed catalog refresh retains its previous snapshot and
  keeps the Server degraded until the failed catalog recovers.
- Resource reads and Prompt results enforce the configured byte budget. Prompt
  arguments are validated locally before a request is sent.
- `roots/list` returns only the roots explicitly authorized for that Server;
  the Host does not infer roots from another Server or from the workspace.
- Resource subscriptions accept updates only for URIs that were explicitly
  subscribed, and catalog change notifications are refresh-coalesced.
- List-changed storms are coalesced, while progress and logging storms are
  capped per Server.
- Output schema, annotations, structured arrays, unknown content blocks, and
  protocol cancellation have deterministic Fake Server or fake transport
  coverage.
- UI coverage verifies MCP menu routing, Server state and capability
  presentation, lifecycle controls, tool policy details, diagnostics, and
  sensitive-text redaction.

## Completion and Logging Boundaries

- Completion requests are sent only after the negotiated version and the
  `completions` capability are present. Prompt names, Prompt argument names,
  Resource template URIs, context arguments, and partial values are checked
  locally before a request is sent.
- Completion results preserve `values`, optional `total`, optional `hasMore`,
  and unknown fields in the bounded result object. They are exposed to the
  control panel and are not inserted into Agent context automatically.
- Logging levels are restricted to `debug`, `info`, `notice`, `warning`,
  `error`, `critical`, `alert`, and `emergency`. A Server without `logging`
  capability is rejected locally, and the last successfully applied level is
  tracked independently for each Server.
- Server `instructions` are capped at 4 KiB per Server and 8 KiB combined.
  `useInstructions` is persisted per Server and defaults to `true` when absent
  from older configuration files. Instructions never override local tool
  policy, authorization roots, or safety checks.

## Offline Stdio Compatibility Matrix

The default fixture suite is offline and does not download or launch external
software. It exercises the same framing rules used by native, Python-style,
and Node-style stdio Servers:

| Fixture style | Coverage |
| --- | --- |
| Native LF | JSON-RPC request/response and lifecycle |
| Python-style CRLF | CRLF framing and lifecycle |
| Node-style fragmented CRLF | JSON split across multiple stdout reads |
| Windows `.cmd` wrapper | `cmd.exe` wrapper startup, environment inheritance, and shutdown |

The suite also covers absolute program paths, working-directory and explicit
environment checks, stderr storms, stdout startup noise, a single stdout
message over 1 MiB, abnormal exit, timeout, cancellation, and process stop.
stdout startup noise and malformed or oversized messages fail that Server;
stderr remains diagnostic data and cannot become JSON-RPC input. Supported
protocol versions have dedicated negotiation coverage and share only the
implemented stdio/core feature subset.
