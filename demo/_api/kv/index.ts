export function list(args: {prefix?: string, count?: string}): object {
  const prefix = args.prefix || "";
  const count = parseInt(args.count || "200") || 200;
  const end = prefix + "\x7f";
  const results = kv.range(prefix, end, count);
  return results.map((e: any) => ({ key: e.key, size: e.value_size }));
}

export function get(args: {key: string}): object {
  const value = kv.get(args.key);
  if (value === null) {
    response.status(404);
    return { error: "not found" };
  }
  return { key: args.key, value: value };
}
