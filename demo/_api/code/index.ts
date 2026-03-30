export function list(): object {
  return code.list();
}

export function get(args: {path: string}): object {
  const source = code.get(args.path);
  if (source === null) {
    response.status(404);
    return { error: "not found" };
  }
  return { path: args.path, content: source };
}

export function save(args: {path: string, content: string}): object {
  code.put(args.path, args.content);
  return { ok: true, path: args.path };
}

export function del(args: {path: string}): object {
  code.delete(args.path);
  return { ok: true, path: args.path };
}
