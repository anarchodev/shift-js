# shift-js

A high-performance HTTP/2 server that executes JavaScript handlers via QuickJS. Modules are stored in a SQLite key-value store and compiled to bytecode on first request. Each request runs in an isolated, snapshot-restored JS runtime on a per-core worker thread with zero shared state.

## Quick start

```bash
# Build (requires CMake and a C23 compiler)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# Seed a module
./build/sjsctl -d app.db put "__code/index.mjs" \
  'export function index(args) { return { hello: "world", args }; }
   export function greet(args) { return "Hello, " + args.name + "!"; }'

# Start the server
./build/shift-js -d app.db -p 9000 -w 4

# Call it
curl --http2-prior-knowledge http://localhost:9000/
# → {"hello":"world","args":{}}

curl --http2-prior-knowledge 'http://localhost:9000/greet?name=World'
# → Hello, World!
```

## How modules work

Modules are JavaScript ES modules stored in SQLite under `__code/` keys. There are two kinds of modules, distinguished automatically by what they export.

### API modules (`.mjs`)

Export named functions. The URL path determines which function to call:

```
GET /math/add?a=1&b=2  →  calls add({a: "1", b: "2"}) in __code/math/index.mjs
POST /math/add          →  calls add(<parsed JSON body>) in __code/math/index.mjs
```

**Routing**: the last path segment is the function name, everything before it resolves the module. `/foo/bar` tries `__code/foo/bar/index.mjs` first (full path as module), then falls back to `__code/foo/index.mjs` with function `bar`. If no function name is given (e.g. `GET /`), the `index` export is called.

**GET/HEAD**: arguments come from query string parameters, passed as an object to the function.

**POST/PUT/PATCH/DELETE**: arguments come from the JSON request body, passed as an object to the function.

**Return values**: strings are sent as-is. Objects/arrays are JSON-stringified with `content-type: application/json`. Null/undefined produce an empty body.

**JSONP**: add `?callback=myFunc` to any GET request to wrap the response as `myFunc(<json>);` with `content-type: application/javascript`.

```javascript
// __code/index.mjs
export function index(args) {
    return { status: "ok", args };
}

export function greet(args) {
    return "Hello, " + (args.name || "stranger") + "!";
}

export function createUser(args) {
    kv.put("users/" + args.id, JSON.stringify(args));
    return { created: args.id };
}
```

### Page modules (`.ejs`)

EJS templates that export a `__render` function. Detected automatically -- if a module exports `__render`, it uses page semantics regardless of HTTP method.

Tags: `<%= expr %>` (HTML-escaped output), `<%- expr %>` (raw output), `<% code %>` (execute), `<%# comment %>` (ignored).

The `request` and `kv` globals are available inside templates. Response `content-type` is automatically set to `text/html`.

```html
<!-- __code/dashboard/index.ejs -->
<html>
<body>
  <h1>Dashboard</h1>
  <p>Method: <%= request.method %>, Path: <%= request.path %></p>
  <% let items = JSON.parse(kv.get("items") || "[]"); %>
  <ul>
    <% for (let item of items) { %>
      <li><%= item.name %></li>
    <% } %>
  </ul>
</body>
</html>
```

### Priority

When both `index.ejs` and `index.mjs` exist at the same path, `.mjs` is checked first. The first match wins.

## JS globals

Available in all modules:

| Global | Description |
|--------|-------------|
| `request.method` | HTTP method (GET, POST, etc.) |
| `request.path` | Request path including query string |
| `request.body` | Request body as string, or null |
| `request.headers` | Object of lowercase header names to values |
| `response.status(code)` | Set HTTP status code (default: 200) |
| `response.header(name, value)` | Add a response header |
| `kv.get(key)` | Get value from KV store (returns string or null) |
| `kv.put(key, value)` | Store a value |
| `kv.delete(key)` | Delete a key |
| `kv.range(start, end, count?)` | Range query, returns `[{key, value, value_size}]` |
| `session.id` | Read-only session ID (auto-generated if no `_sjs_sid` cookie) |
| `session.get(key)` | Get a value from the session |
| `session.set(key, value)` | Set a session value (persisted to KV on request end) |
| `session.delete(key)` | Delete a session key |
| `crypto.getRandomValues(buf)` | Fill a TypedArray with random bytes |
| `crypto.randomUUID()` | Generate a UUID v4 string |
| `crypto.subtle.digest(algo, data)` | Hash data (SHA-256, SHA-384, SHA-512) |
| `crypto.subtle.importKey(...)` | Import a cryptographic key (HMAC) |
| `crypto.subtle.sign(...)` | Sign data with HMAC |
| `crypto.subtle.verify(...)` | Verify an HMAC signature |
| `crypto.subtle.deriveBits(...)` | Derive bits (HKDF) |

## Server options

```
./build/shift-js [options]
  -d <path>     Database file (default: shift-js.db)
  -p <port>     Listen port (default: 9000)
  -w <workers>  Worker threads (default: CPU count, max: CPU count)
  -t            Enable TLS

Raft clustering (enables when --raft-id is set):
  --raft-id <N>          This node's index (0-based)
  --raft-peers <addrs>   Comma-separated host:port for all nodes
  --raft-port <port>     Port for Raft peer TCP (default: 9100)
  --batch-ms <ms>        Proposal batch interval (default: 2)
  --batch-max <N>        Max proposals per batch (default: 256)
```

## sjsctl

CLI tool for managing the KV store and modules.

```bash
sjsctl -d <db> put <key> <value>          # Store a value
sjsctl -d <db> putfile <key> <file>       # Store file contents
sjsctl -d <db> get <key>                  # Retrieve a value
sjsctl -d <db> delete <key>               # Delete a key
sjsctl -d <db> range <start> <end>        # Range query
sjsctl -d <db> list                       # List all keys
sjsctl -d <db> upload <dir>               # Upload directory as __code/ modules
sjsctl -d <db> import <dir>               # Import directory as raw KV entries
sjsctl -d <db> export <dir>               # Export KV store to directory
sjsctl -d <db> watch <dir>                # Watch directory and auto-upload changes
```

**Tenant operations** (add `-t <tenant_id>`):

```bash
sjsctl -d <db> domain-map <host> <id>     # Map hostname to tenant
sjsctl -d <db> domain-unmap <host>        # Remove domain mapping
sjsctl -d <db> cert-put <name> <cert> <key>  # Store TLS certificate
```

## TLS

```bash
./build/sjsctl -d app.db cert-put default cert.pem key.pem
./build/shift-js -d app.db -p 9443 -w 4 -t
```

Per-tenant TLS certificates are supported via SNI. The server resolves hostname to tenant ID and loads the corresponding certificate.

## Multitenancy

Tenants are isolated by KV prefix scoping. All KV operations for a tenant are transparently prefixed with `tenants/<id>/`, including module source, compiled bytecode, and application data.

```bash
# Create modules for tenant 42
./build/sjsctl -d app.db -t 42 put "__code/index.mjs" 'export function index() { return "tenant 42"; }'

# Map a domain to tenant 42
./build/sjsctl -d app.db domain-map example.com 42
```

## Clustering

shift-js supports multi-node clustering via Raft consensus. When enabled, KV writes are replicated across all nodes.

```bash
# Start a 3-node cluster
./build/shift-js -d node0.db -p 9000 --raft-id 0 \
  --raft-peers "127.0.0.1:9100,127.0.0.1:9101,127.0.0.1:9102" --raft-port 9100

./build/shift-js -d node1.db -p 9001 --raft-id 1 \
  --raft-peers "127.0.0.1:9100,127.0.0.1:9101,127.0.0.1:9102" --raft-port 9101

./build/shift-js -d node2.db -p 9002 --raft-id 2 \
  --raft-peers "127.0.0.1:9100,127.0.0.1:9101,127.0.0.1:9102" --raft-port 9102
```

Workers propose KV write-sets to the Raft leader, which replicates them to followers before committing. New followers catch up via incremental KV snapshots. The Raft thread runs on the last CPU core; worker count is reduced by one when clustering is enabled.

## Architecture

- **One worker thread per CPU core**, each with its own SQLite connection, QuickJS runtime, and io_uring instance. No shared mutable state.
- **Snapshot-based runtime isolation**: a frozen JS runtime template is created via a two-address diff algorithm (init in two arenas, diff to find pointers vs data). Per-request restore is a memcpy + bitmap-driven pointer relocation. Per-request JS runs on a 10MB bump-allocated arena that is reset wholesale after the request (no GC).
- **Bytecode caching**: modules are compiled once and cached in the KV store. Subsequent requests load pre-compiled bytecode.
- **Transactional KV**: each request runs inside a SQLite transaction with automatic retry on conflicts.
- **Raft replication**: optional clustering where KV mutations are proposed to a leader, batched, replicated via log entries, and committed. Followers receive incremental snapshots for catch-up.

## Dependencies

All fetched automatically via CMake `FetchContent`:

- [shift](https://github.com/) -- ECS framework
- [shift-h2](https://github.com/) -- HTTP/2 server (io_uring)
- [quickjs-ng](https://github.com/nicbarker/quickjs-ng) -- JavaScript engine (patched for deterministic shape hashing)
- [SQLite](https://sqlite.org/) -- database (WAL mode)
- [dmon](https://github.com/nicbarker/dmon) -- filesystem watcher (sjsctl watch)
- [raft](https://github.com/willemt/raft) -- Raft consensus
- [OpenSSL](https://www.openssl.org/) -- crypto primitives (WebCrypto API)

## MCP server

A Node.js MCP server for programmatic access to a running shift-js instance.

```bash
cd mcp && npm install
SHIFT_JS_URL=http://localhost:9000 npx shift-js-mcp
```

Provides tools for module CRUD, KV access, tenant management, domain mapping, and endpoint testing.

## License

AGPL-3.0
