export default function() {
  return (
    <html>
    <head>
      <meta charset="utf-8" />
      <title>Requests - shift-js</title>
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
      <div class="split">
        <div class="sidebar" style="width:100%;max-width:100%">
          <div class="sidebar-header">
            <span style="font-size:13px;font-weight:600">Recent Requests</span>
            <button class="btn" id="refresh-btn">Refresh</button>
          </div>
          <div class="sidebar-list" id="request-list"></div>
        </div>
      </div>
      <div id="modal-root"></div>
      <script src="/requests.js"></script>
    </body>
    </html>
  );
}
