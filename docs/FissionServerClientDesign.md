# Fission.Server Client Design

## Status

Proposed. This document describes a small C++23 client for the existing `Fission.Server` HTTP API. It also records the wire contract needed by clients written in other languages.

## Problem

`Fission.Server` exposes decompilation through HTTP, but callers must currently build JSON, encode bytecode, interpret status codes, and validate response shapes themselves. That repeats protocol details in every consumer and makes API drift easy to miss.

## Decision

Add a static `Fission.Client` library with one synchronous, typed client. Use Boost.Beast and Boost.JSON, which the repository already depends on. Keep the wire contract public so non-C++ callers can implement it without linking the library.

First release will provide:

- health and service-information queries;
- vanilla and Roblox bytecode decompilation;
- supported decompiler flags;
- optional CFG and AST capture;
- explicit transport, HTTP, protocol, and server errors;
- caller-controlled deadlines and response-size limits;
- safe concurrent use from multiple threads.

First release will not provide:

- server process management or service discovery;
- batching, streaming, WebSockets, or persistent connections;
- automatic retries;
- authentication or TLS;
- generated clients for other languages;
- an asynchronous wrapper. Callers that need asynchronous work can schedule the blocking call on their existing executor.

## Current wire contract

Server listens on `127.0.0.1` only. Default port is `8080`. Default maximum server-side decompilation budget is 30 seconds. Server accepts one request per connection, writes one response, then closes the connection.

### Endpoints

| Method | Path | Success | Purpose |
| --- | --- | --- | --- |
| `GET` | `/` | `200` | Service name, version, description, and endpoint summary |
| `GET` | `/health` | `200` | Health state and process uptime |
| `POST` | `/decompile` | `200` | Decompile base64-encoded bytecode |

Unknown routes and wrong methods return `404`, not `405`.

### Decompile request

```json
{
  "mode": "vanilla",
  "bytecode": "<base64>",
  "flags": {
    "inferTypes": true,
    "optimizeIR": true,
    "inferRobloxTypes": false,
    "autoNameVariables": true,
    "omitFissionComments": false
  },
  "outputs": ["cfg", "ast"],
  "timeout": 20
}
```

Fields:

| Field | Required | Meaning |
| --- | --- | --- |
| `mode` | yes | `"vanilla"` or `"roblox"` |
| `bytecode` | yes | Base64-encoded bytecode bytes |
| `flags` | no | Boolean decompiler options |
| `outputs` | no | Any combination of `"cfg"` and `"ast"` |
| `timeout` | no | Positive integer server budget in seconds, capped by server configuration |

Client must expose only flags that affect an HTTP result:

- `inferTypes`
- `optimizeIR`
- `inferRobloxTypes`
- `autoNameVariables`
- `omitFissionComments`

Server accepts but strips `printIR`, `writeIRToFile`, `generateIRGraph`, `generateSSAIRGraph`, and `printTimingBreakdown`. Client must not send or expose those flags. CFG and AST use `outputs`, not file-producing flags.

Unknown output names and non-string output entries are currently ignored. Invalid, zero, or negative timeout values fall back to server maximum. Client should reject these values before sending because silent fallback is surprising.

HTTP request body is limited to 32 MiB. Client must serialize request first and reject any body over that limit. A raw-byte limit alone is insufficient because Base64 and JSON add overhead.

### Successful decompile response

```json
{
  "ok": true,
  "result": {
    "decompilationOutput": "return 1\n",
    "irOutput": "",
    "timingStatistics": "",
    "resultCode": "Success",
    "cfg": "digraph { ... }",
    "ast": {}
  }
}
```

`cfg` and `ast` appear only when requested and produced. CFG is DOT text. AST is normally a JSON value; current server can fall back to a raw string if its stored AST text cannot be parsed. Client must represent AST as `boost::json::value` and accept a JSON string without treating it as a protocol failure.

`irOutput` and `timingStatistics` remain in the response schema but are normally empty because server strips their output-producing flags.

### Error response

```json
{
  "ok": false,
  "error": "Invalid base64 encoding"
}
```

| Status | Meaning |
| --- | --- |
| `400` | Malformed JSON, missing required field, invalid mode, or invalid Base64 |
| `404` | Unknown route or wrong method |
| `422` | Bytecode could not be deserialized |
| `500` | Internal decompilation failure |

Oversized or incomplete HTTP bodies can fail before route handling and may close the connection without a JSON error. Client must classify that as a transport failure unless an HTTP response was received.

### Health response

```json
{
  "ok": true,
  "status": "healthy",
  "uptime_seconds": 12.5
}
```

Health confirms that HTTP worker can answer. It does not prove that arbitrary bytecode will decompile.

## Proposed C++ API

Public header: `Fission.Client/include/Fission/Client.hpp`

```cpp
#pragma once

#include <boost/json/value.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace Fission::Client {
    enum class BytecodeMode {
        Vanilla,
        Roblox,
    };

    enum class ErrorKind {
        InvalidRequest,
        Transport,
        Timeout,
        Http,
        Protocol,
        Server,
    };

    struct ClientError {
        ErrorKind kind;
        std::string message;
        std::optional<unsigned int> httpStatus;
    };

    struct ClientConfig {
        std::string address = "127.0.0.1";
        std::uint16_t port = 8080;
        std::chrono::seconds connectTimeout{5};
        std::chrono::seconds requestTimeout{35};
        std::size_t maxResponseBytes = 64u * 1024u * 1024u;
        bool allowInsecureRemote = false;
    };

    struct DecompileOptions {
        BytecodeMode mode = BytecodeMode::Vanilla;
        bool inferTypes = false;
        bool optimizeIR = false;
        bool inferRobloxTypes = false;
        bool autoNameVariables = false;
        bool omitFissionComments = false;
        bool captureCFG = false;
        bool captureAST = false;
        std::optional<std::chrono::seconds> serverTimeout;
    };

    struct DecompileResult {
        std::string source;
        std::string ir;
        std::string timingStatistics;
        std::optional<std::string> cfg;
        std::optional<boost::json::value> ast;
    };

    struct Health {
        std::string status;
        double uptimeSeconds;
    };

    struct ServiceInfo {
        std::string name;
        std::string version;
        std::string description;
    };

    class Client {
      public:
        explicit Client(ClientConfig config = {});

        [[nodiscard]] std::expected<ServiceInfo, ClientError> GetServiceInfo() const;
        [[nodiscard]] std::expected<Health, ClientError> GetHealth() const;
        [[nodiscard]] std::expected<DecompileResult, ClientError>
        Decompile(std::span<const std::byte> bytecode, const DecompileOptions &options = {}) const;

      private:
        ClientConfig m_Config;
    };
}
```

`std::expected` keeps failure handling explicit and matches C++23 without adding an exception hierarchy. `Client` stores immutable connection settings only. Each call owns its resolver, socket, buffers, and parser, so one client instance may be called concurrently.

### Example

```cpp
Fission::Client::Client client;

Fission::Client::DecompileOptions options;
options.mode = Fission::Client::BytecodeMode::Vanilla;
options.captureAST = true;
options.serverTimeout = std::chrono::seconds{20};

auto result = client.Decompile(bytecode, options);
if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
}

std::cout << result->source;
```

## Request flow

1. Validate numeric IP address, port, non-empty bytecode, positive server timeout, and configured size limits.
2. Base64-encode bytecode.
3. Build JSON with required fields. Omit false flags and unrequested outputs.
4. Serialize JSON and reject bodies over 32 MiB.
5. Connect to numeric IP address within `connectTimeout`.
6. Send `POST /decompile` with `Content-Type: application/json`.
7. Read response within the remaining `requestTimeout`, enforcing `maxResponseBytes` in Beast parser.
8. Parse JSON and validate required fields for selected response branch.
9. Return typed result or `ClientError`.

`GetServiceInfo` and `GetHealth` use same transport path with `GET` requests.

## Error mapping

| Condition | `ErrorKind` | HTTP status retained |
| --- | --- | --- |
| Empty bytecode, invalid timeout, invalid config, oversized request | `InvalidRequest` | no |
| Address parsing, connect, write, read, or early-close failure | `Transport` | no |
| Connect or request deadline expires | `Timeout` | no |
| Non-2xx response without valid Fission error body | `Http` | yes |
| Invalid JSON or missing/wrong-typed required response field | `Protocol` | if available |
| Valid `{ "ok": false, "error": ... }` response | `Server` | yes |

Parser must ignore unknown object fields for forward compatibility. It must not convert malformed success payloads into empty results.

No automatic retry. Retrying `POST /decompile` can duplicate expensive work after client loses response, and current protocol has no request identifier or cancellation. Caller may retry based on its own workload policy.

## Timeout rules

Server timeout and client timeout solve different problems:

- `serverTimeout` limits decompiler work and is sent in request JSON.
- `connectTimeout` limits connection establishment.
- `requestTimeout` is one absolute deadline for the full HTTP exchange from write through response read. It is not reset between operations.

`requestTimeout` should exceed requested server timeout by enough time to transmit and parse response. Defaults assume standard server maximum: 30-second server budget and 35-second client deadline. Deployments that raise `--max-timeout` must also raise client request timeout.

Closing timed-out client socket does not cancel server-side decompilation. True cancellation requires a later protocol change.

## Security boundary

Current server has no TLS, authentication, authorization, or rate limiting and binds only to loopback. Default client must therefore target `127.0.0.1:8080`.

First release accepts numeric IP addresses, not hostnames. This avoids an unbounded synchronous DNS lookup and matches current loopback deployment. If caller configures a non-loopback address over plain HTTP, client should require explicit `allowInsecureRemote = true` in `ClientConfig`. This prevents accidental remote bytecode disclosure while preserving deliberate reverse-proxy or private-network use. A future HTTPS transport can add hostname resolution.

Client logs may include method, route, status, elapsed time, and byte counts. They must not include bytecode, generated source, AST, CFG, or complete server error bodies by default.

## Compatibility

Current root response reports product version `1.0.0`, not a distinct protocol version. Initial client will target current schema and tolerate added fields. It should reject missing required fields and incompatible types.

Before publishing client as a stable external package, server should add an integer `apiVersion` to `GET /` while keeping existing fields. Path versioning is unnecessary until incompatible protocol revision exists.

Current error body contains human-readable text only. Client can expose status and text, but should not derive machine-readable error codes by matching messages. A later server revision may add `errorCode` while retaining `error` for compatibility.

## Build layout

Minimum repository changes for implementation:

```text
Fission.Client/
  include/Fission/Client.hpp
  src/Client.cpp
Fission.Tests/Client/
  ClientBehaviour.cpp
CMakeLists.txt
```

`Fission.Client` links `Boost::beast` and `Boost::json`. It does not link `Fission.Decompiler`; protocol client should not pull decompiler implementation into consuming programs.

## Verification

### Unit and contract tests

- request encodes exact mode, safe flags, outputs, timeout, and Base64 data;
- request rejects empty bytecode, non-positive timeout, and serialized body over 32 MiB;
- response parses service, health, minimal success, CFG, structured AST, string AST, and server errors;
- malformed JSON and missing or wrong-typed required fields return `Protocol`;
- every documented HTTP status maps to expected error kind without message matching;
- unknown response fields are ignored;
- response parser enforces configured size limit;
- concurrent calls do not share sockets or parser state.

Use a one-request loopback Beast fixture for transport tests. It can capture request and return fixed response without adding a transport interface used only by tests.

### End-to-end test

Start `Fission.Server` on an unused loopback port, then verify:

1. health query succeeds;
2. valid vanilla bytecode produces source;
3. same request with CFG and AST returns both;
4. invalid bytecode returns `Server` with status `422`;
5. server process exits after test cleanup.

Existing `ServerBehaviour.cpp` tests remain source of truth for route handling. Client tests cover serialization, sockets, deadlines, and response parsing.

## Delivery order

1. Add protocol types, synchronous client, and CMake target.
2. Add loopback transport tests and end-to-end server smoke test.
3. Add a short usage section to `README.md` after implementation passes on Windows and Linux.

Stop after synchronous client. Add async API, HTTPS, pooling, cancellation, or generated language SDKs only when a concrete consumer requires them.

## Acceptance criteria

- Consumer decompiles vanilla and Roblox bytecode without constructing JSON or Base64 manually.
- All current safe flags and optional outputs are typed.
- Invalid input fails before network I/O.
- Network, timeout, HTTP, protocol, and server failures remain distinguishable.
- Client accepts current server responses and ignores additive fields.
- One client instance supports concurrent calls without shared mutable request state.
- Client introduces no dependency not already present in repository.
- Windows and Linux builds and tests pass.
