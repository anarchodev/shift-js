export default function() {
  return (
    <html>
    <head>
      <meta charset="utf-8" />
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
            <button class="btn btn-primary" id="save-btn" style="margin-left:auto;font-size:13px;padding:4px 16px;display:none" onclick="saveKey()">Save</button>
          </div>
          <div class="value-pane" id="value-pane">
            <textarea id="value-display" style="min-height:100px;width:100%;height:100%;background:transparent;color:#e6edf3;border:none;outline:none;resize:none;font-family:monospace;font-size:13px;padding:0" spellcheck="false">Click a key to view its value</textarea>
          </div>
        </div>
      </div>
      <script src="/kv-viewer.js"></script>
    </body>
    </html>
  );
}
