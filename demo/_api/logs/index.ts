export function listRequests(args: {limit?: string}): object {
  const limit = parseInt(args.limit || "100") || 100;
  return logs.requests({ limit });
}

export function getLogs(args: {request_id: string}): object {
  return logs.query({ request_id: args.request_id, limit: 100 });
}

export function getReplay(args: {request_id: string}): object {
  const data = logs.replay(args.request_id);
  if (!data) {
    response.status(404);
    return { error: "replay not found" };
  }
  return data;
}

export function getBlob(args: {hash: string}): object {
  const content = kv.get("__code_blob/" + args.hash);
  if (content === null) {
    response.status(404);
    return { error: "blob not found" };
  }
  return { content };
}

export function getTranspiled(args: {path: string}): object {
  const js = kv.get("__code_js/" + args.path);
  if (js === null) {
    response.status(404);
    return { error: "transpiled source not found" };
  }
  return { content: js };
}

export function getSourceMap(args: {path: string}): object {
  const sm = kv.get("__code_sourcemap/" + args.path);
  if (sm === null) {
    response.status(404);
    return { error: "source map not found" };
  }
  return { sourceMap: sm };
}
