export default function() {
  return (
    <html>
    <head>
      <title>Code Editor - shift-js</title>
      <link rel="stylesheet" href="/style.css" />
      <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/codemirror.min.css" />
      <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/codemirror.min.js"></script>
      <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/mode/javascript/javascript.min.js"></script>
      <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/mode/xml/xml.min.js"></script>
      <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/mode/jsx/jsx.min.js"></script>
      <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/mode/htmlmixed/htmlmixed.min.js"></script>
      <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.18/mode/css/css.min.js"></script>
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
            <span style="font-size:13px;font-weight:600">Files</span>
            <button class="btn" onclick="createFile()" style="margin-left:auto;font-size:12px">+ New</button>
          </div>
          <div class="sidebar-list" id="file-list"></div>
        </div>
        <div class="main">
          <div class="toolbar">
            <span class="filename" id="current-file">Select a file</span>
            <button class="btn btn-primary" onclick="saveFile()" id="save-btn" disabled>Save</button>
            <button class="btn btn-danger" onclick="deleteFile()" id="delete-btn" disabled>Delete</button>
          </div>
          <div class="editor-wrap" id="editor-wrap"></div>
        </div>
      </div>
      <script src="/editor.js"></script>
    </body>
    </html>
  );
}
