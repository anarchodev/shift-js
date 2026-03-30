export default function() {
  return (
    <html>
    <head>
      <title>shift-js demo</title>
      <link rel="stylesheet" href="/style.css" />
    </head>
    <body>
      <div class="nav">
        <h1>shift-js</h1>
        <a href="/editor">Code Editor</a>
        <a href="/kv">KV Viewer</a>
        <a href="/logs">Requests</a>
      </div>
      <div class="container" style="text-align:center;padding-top:80px">
        <h2>Welcome to the shift-js demo</h2>
        <p style="color:#888;max-width:500px;margin:20px auto">
          A high-performance HTTP/2 server with JavaScript handlers,
          KV store, and deterministic request replay.
        </p>
        <div style="display:flex;gap:20px;justify-content:center;margin-top:40px">
          <a href="/editor" class="card">
            <h3>Code Editor</h3>
            <p>Browse and edit modules with syntax highlighting</p>
          </a>
          <a href="/kv" class="card">
            <h3>KV Viewer</h3>
            <p>Explore the key-value store</p>
          </a>
          <a href="/logs" class="card">
            <h3>Requests</h3>
            <p>View requests and replay with debugger</p>
          </a>
        </div>
      </div>
    </body>
    </html>
  );
}
