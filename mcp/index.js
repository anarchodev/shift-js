#!/usr/bin/env node

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const BASE_URL = process.env.SHIFT_JS_URL || "http://localhost:9000";

async function api(path, method = "GET", body) {
  const opts = {
    method,
    headers: { "content-type": "application/json" },
  };
  if (body !== undefined) {
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(`${BASE_URL}${path}`, opts);
  const text = await res.text();
  try {
    return { status: res.status, data: JSON.parse(text) };
  } catch {
    return { status: res.status, data: text };
  }
}

function text(content) {
  return { content: [{ type: "text", text: typeof content === "string" ? content : JSON.stringify(content, null, 2) }] };
}

function error(msg) {
  return { content: [{ type: "text", text: msg }], isError: true };
}

const server = new McpServer({
  name: "shift-js",
  version: "1.0.0",
});

// --- Modules ---

server.tool(
  "list_modules",
  "List all JavaScript/EJS modules deployed on the server",
  {},
  async () => {
    const { data } = await api("/_api/modules");
    return text(data);
  }
);

server.tool(
  "read_module",
  "Read the source code of a deployed module",
  { path: z.string().describe("Module path, e.g. index.mjs or blog/index.ejs") },
  async ({ path }) => {
    const { status, data } = await api(`/_api/modules?path=${encodeURIComponent(path)}`);
    if (status === 404) return error(`Module not found: ${path}`);
    return text(data.content);
  }
);

server.tool(
  "write_module",
  "Create or update a module. Automatically invalidates the bytecode cache.",
  {
    path: z.string().describe("Module path, e.g. index.mjs or blog/index.ejs"),
    content: z.string().describe("The full source code of the module"),
  },
  async ({ path, content }) => {
    const { data } = await api("/_api/modules", "POST", { path, content });
    return text(data);
  }
);

server.tool(
  "delete_module",
  "Delete a deployed module and its bytecode cache",
  { path: z.string().describe("Module path to delete") },
  async ({ path }) => {
    const { data } = await api("/_api/modules", "DELETE", { path });
    return text(data);
  }
);

// --- KV Store ---

server.tool(
  "kv_list",
  "List keys in the KV store, optionally filtered by prefix",
  { prefix: z.string().optional().describe("Key prefix filter") },
  async ({ prefix }) => {
    const qs = prefix ? `?prefix=${encodeURIComponent(prefix)}` : "";
    const { data } = await api(`/_api/kv${qs}`);
    return text(data);
  }
);

server.tool(
  "kv_get",
  "Get the value of a key from the KV store",
  { key: z.string().describe("The key to retrieve") },
  async ({ key }) => {
    const { status, data } = await api(`/_api/kv?key=${encodeURIComponent(key)}`);
    if (status === 404) return error(`Key not found: ${key}`);
    return text(data.value);
  }
);

server.tool(
  "kv_put",
  "Set a key-value pair in the KV store",
  {
    key: z.string().describe("The key to set"),
    value: z.string().describe("The value to store"),
  },
  async ({ key, value }) => {
    const { data } = await api("/_api/kv", "POST", { key, value });
    return text(data);
  }
);

server.tool(
  "kv_delete",
  "Delete a key from the KV store",
  { key: z.string().describe("The key to delete") },
  async ({ key }) => {
    const { data } = await api("/_api/kv", "DELETE", { key });
    return text(data);
  }
);

// --- Tenants ---

server.tool(
  "list_tenants",
  "List all tenants",
  {},
  async () => {
    const { data } = await api("/_api/tenants");
    return text(data);
  }
);

server.tool(
  "create_tenant",
  "Create a new tenant with a default landing page",
  { name: z.string().describe("Tenant name") },
  async ({ name }) => {
    const { data } = await api("/_api/tenants", "POST", { name });
    return text(data);
  }
);

server.tool(
  "delete_tenant",
  "Delete a tenant",
  { id: z.number().describe("Tenant ID") },
  async ({ id }) => {
    const { data } = await api("/_api/tenants", "DELETE", { id });
    return text(data);
  }
);

// --- Domains ---

server.tool(
  "list_domains",
  "List all domain-to-tenant mappings",
  {},
  async () => {
    const { data } = await api("/_api/domains");
    return text(data);
  }
);

server.tool(
  "map_domain",
  "Map a hostname to a tenant",
  {
    host: z.string().describe("Hostname, e.g. example.com"),
    tenant_id: z.string().describe("Tenant ID to map to"),
  },
  async ({ host, tenant_id }) => {
    const { data } = await api("/_api/domains", "POST", { host, tenant_id });
    return text(data);
  }
);

server.tool(
  "unmap_domain",
  "Remove a domain-to-tenant mapping",
  { host: z.string().describe("Hostname to unmap") },
  async ({ host }) => {
    const { data } = await api("/_api/domains", "DELETE", { host });
    return text(data);
  }
);

// --- Test Endpoint ---

server.tool(
  "fetch_endpoint",
  "Make an HTTP request to any path on the server and return the response",
  {
    method: z.enum(["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]).default("GET").describe("HTTP method"),
    path: z.string().describe("URL path, e.g. / or /blog"),
    body: z.string().optional().describe("Request body (for POST/PUT/PATCH)"),
  },
  async ({ method, path, body }) => {
    const opts = { method, headers: {} };
    if (body && ["POST", "PUT", "PATCH"].includes(method)) {
      opts.headers["content-type"] = "application/json";
      opts.body = body;
    }
    const res = await fetch(`${BASE_URL}${path}`, opts);
    const responseBody = await res.text();
    const headers = {};
    res.headers.forEach((v, k) => { headers[k] = v; });
    return text({
      status: res.status,
      headers,
      body: responseBody,
    });
  }
);

// --- Start ---

const transport = new StdioServerTransport();
await server.connect(transport);
