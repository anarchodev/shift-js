export default function() {
  const qs = request.path.split('?')[1] || '';
  const params: Record<string, string> = {};
  for (const pair of qs.split('&')) {
    const [k, v] = pair.split('=');
    if (k) params[k] = decodeURIComponent(v || '');
  }
  const requestId = params.request_id || '';

  return (
    <html>
    <head>
      <meta charset="utf-8" />
      <title>Replay - shift-js</title>
      <link rel="stylesheet" href="/style.css" />
    </head>
    <body>
      <div class="nav">
        <h1>shift-js</h1>
        <a href="/">Home</a>
        <a href="/editor">Code Editor</a>
        <a href="/kv">KV Viewer</a>
        <a href="/logs">Requests</a>
      </div>
      <div class="container">
        <div class="replay-info">
          <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
            <h2 style="font-size:18px">Request Replay</h2>
            <span style="font-family:monospace;color:#8b949e" id="replay-rid">{requestId}</span>
            <button class="btn btn-primary" id="replay-btn" disabled style="margin-left:auto;font-size:14px;padding:8px 24px">
              Replay
            </button>
          </div>
          <div id="request-info">Loading...</div>
          <div id="replay-status"></div>
          <div style="margin-top:16px;padding:12px;background:#161b22;border:1px solid #30363d;border-radius:6px">
            <h3 style="font-size:14px;margin-bottom:8px;color:#8b949e">How to debug</h3>
            <ol style="font-size:13px;color:#8b949e;padding-left:20px;line-height:1.8">
              <li>Open DevTools (F12) and go to the <strong>Sources</strong> tab</li>
              <li>Find the source files under the <code>(no domain)</code> section</li>
              <li>Set a breakpoint in the handler function</li>
              <li>Click the <strong>Replay</strong> button above</li>
              <li>Step through the code with the original request context</li>
            </ol>
          </div>
          <div id="module-list" style="margin-top:16px"></div>
          <h3 style="font-size:14px;margin-top:16px;margin-bottom:8px;color:#8b949e">Replay Output</h3>
          <pre id="replay-output" style="font-family:monospace;font-size:13px;background:#161b22;border:1px solid #30363d;border-radius:6px;padding:12px;min-height:60px;color:#e6edf3">Click Replay to execute</pre>
        </div>
      </div>
      <script>{"var REQUEST_ID = '" + requestId + "';"}</script>
      <script src="/replay.js"></script>
    </body>
    </html>
  );
}
