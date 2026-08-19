# MCP Provider Contract for Agent Reliability

## Purpose

qtLLM is a general MCP Host. It must not infer domain behavior from a Server
name, tool name, or business-specific argument name. Reliable Agent execution
therefore depends on a clear boundary:

| Component | Responsibility |
| --- | --- |
| MCP Server | Publish complete tool schemas, annotations, structured results, stable identities, and actionable errors. |
| Model | Interpret the user request and tool contracts, choose the next operation, and decide when a plan step is complete. |
| qtLLM | Validate schemas, enforce policy and ordered execution, preserve evidence, prevent unsafe repeats, and bound recovery. |

These rules apply to ShonDy and any other MCP Server. They do not require a
Server-specific branch in qtLLM.

## Input Schema Requirements

Every tool must provide an `inputSchema` that describes the arguments accepted
by the actual implementation. Descriptions and examples may clarify semantics,
but they do not replace schema constraints.

For an object whose property names are JSON Pointers, constrain the keys with
standard JSON Schema. For example:

```json
{
  "type": "object",
  "properties": {
    "changes": {
      "type": "object",
      "minProperties": 1,
      "propertyNames": {
        "type": "string",
        "pattern": "^(?:/(?:[^~/]|~0|~1)*)+$"
      },
      "additionalProperties": {
        "type": ["number", "string", "boolean", "array", "object"]
      }
    }
  },
  "required": ["changes"],
  "additionalProperties": false
}
```

The key `/density/isotropic/fixedValue` is an argument value. A validator
diagnostic such as `/changes/~1density~1isotropic~1fixedValue` identifies the
location of that key inside the arguments and must never be accepted as the
key itself.

Use `const`, `enum`, discriminated `oneOf`, numeric bounds, string patterns,
and `additionalProperties: false` wherever the implementation has those
constraints. Do not advertise a permissive schema and defer ordinary shape
errors to a generic `INVALID_ARGUMENT` response.

## Instance-Dependent Fields

When valid editable fields depend on the selected object, expose a normal
read-only discovery tool. Its description must state which mutation tool
consumes the result. Return exact paths in structured data:

```json
{
  "ok": true,
  "target": {
    "id": "material-1",
    "type": "fluidMaterial"
  },
  "fields": [
    {
      "path": "/density/isotropic/fixedValue",
      "valueType": "number",
      "currentValue": 900.0
    }
  ]
}
```

The discovery tool may have any name. qtLLM will not look for a tool named
`describe_model`, an argument named `changes`, or selectors named `item_type`
or `name_uuid`. The model chooses the applicable tool from its schema,
description, and prior results.

The mutation implementation must still validate the selected target and exact
field path. On failure, return a stable error code, the rejected key, and the
allowed fields or a clear recovery operation. Do not return only `Invalid operation type, or data type.`

## Structured Results

`structuredContent` is the canonical machine-readable result. It should be
complete enough for the next tool call and for completion evidence. `content`
is for a short human summary or genuinely non-structured content; it should
not contain a second serialized copy of `structuredContent`.

A successful mutation should return:

- an explicit terminal success value such as `ok: true`;
- the requested target name, ID, path, or URI;
- every newly created stable ID or UUID;
- the resulting state needed by the next operation;
- a matching `outputSchema`.

For example, a create or open operation should return its final path and stable
identity directly. The Agent should not need a second status/read-back call
merely to prove a result already present in the mutation response.

Do not return uninitialized numbers, empty template identities, or placeholder
objects as successful business data. If the requested object was not found or
the returned object does not match the selector, return `isError: true` with a
stable domain error code.

## Lifecycle and Errors

Long-running operations must distinguish accepted work from terminal success.
Return a stable job identity and a structured state such as `queued`,
`running`, `succeeded`, `failed`, or `cancelled`. Tool descriptions must make
the corresponding status operation discoverable without relying on a fixed
tool name.

Errors should contain:

- a stable, documented code;
- a concise message;
- the rejected field or selector when relevant;
- whether a mutation may already have occurred;
- structured recovery data when the Server knows the valid alternatives.

## Annotations and Permissions

Publish standard MCP annotations accurately:

- `readOnlyHint: true` for operations that cannot change external state;
- `destructiveHint: true` when an operation can destroy or invalidate state;
- `idempotentHint: true` only when replay is safe;
- `openWorldHint` according to the operation's external interaction.

qtLLM treats annotations as untrusted risk metadata. They improve operation
classification and UI presentation but never grant permission by themselves.

## Acceptance Checklist

- Every dynamic argument key is locally rejectable from `inputSchema`.
- A diagnostic path cannot pass as a JSON Pointer argument key.
- Structured success contains the target identity or location it claims to
  affect.
- `outputSchema` validates every successful structured result.
- `content` does not duplicate the structured payload.
- Read-only, destructive, and idempotent semantics are annotated.
- Running work cannot be mistaken for terminal completion.
- Domain failures provide stable codes and actionable structured details.
- No Host change is needed when the Server renames a tool or domain field.
