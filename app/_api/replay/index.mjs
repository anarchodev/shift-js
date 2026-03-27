export function get() {
  let qs = request.path.split("?")[1] || "";
  let params = {};
  for (let p of qs.split("&")) {
    let [k, v] = p.split("=");
    if (k && v) params[k] = decodeURIComponent(v);
  }

  if (!params.request_id) {
    response.status(400);
    return { error: "request_id is required" };
  }

  let capture = logs.replay(params.request_id);
  if (!capture) {
    response.status(404);
    return { error: "replay data not found" };
  }

  // Resolve source code from blob store using module tree
  let modules = {};
  if (capture.module_tree) {
    for (let mod of capture.module_tree) {
      if (mod.content_hash) {
        let blob = kv.get("__code_blob/" + mod.content_hash);
        if (blob) modules[mod.path] = blob;
      }
    }
  }

  return {
    request: capture.request_data,
    response: capture.response_data,
    kv_tape: capture.kv_tape,
    random_tape: capture.random_tape,
    date_tape: capture.date_tape,
    math_random_tape: capture.math_random_tape,
    module_tree: capture.module_tree,
    modules: modules
  };
}
