export default function() {
  return (
    <html>
    <head>
      <title>KV Viewer - shift-js</title>
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
        <div class="sidebar">
          <div class="sidebar-header">
            <input type="text" id="prefix-input" placeholder="Key prefix..." value="" />
            <button class="btn" onclick="loadKeys()">Go</button>
          </div>
          <div class="sidebar-list" id="key-list"></div>
        </div>
        <div class="main">
          <div class="toolbar">
            <span class="filename" id="current-key">Select a key</span>
          </div>
          <div class="value-pane" id="value-pane">
            <pre id="value-display" style="min-height:100px">Click a key to view its value</pre>
          </div>
        </div>
      </div>
      <script src="/kv-viewer.js"></script>
    </body>
    </html>
  );
}
