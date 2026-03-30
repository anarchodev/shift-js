let editor = null;
let currentPath = null;
let files = [];

function modeForPath(path) {
  if (path.endsWith('.tsx') || path.endsWith('.jsx')) return 'text/jsx';
  if (path.endsWith('.ts') || path.endsWith('.mjs') || path.endsWith('.js')) return 'text/javascript';
  if (path.endsWith('.css')) return 'text/css';
  if (path.endsWith('.html') || path.endsWith('.ejs')) return 'text/html';
  if (path.endsWith('.json')) return 'application/json';
  return 'text/javascript';
}

async function api(fn, args) {
  const resp = await fetch('/_api/code?fn=' + fn + '&' + new URLSearchParams(args));
  return resp.json();
}

async function apiPost(fn, args) {
  const resp = await fetch('/_api/code', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ fn, args })
  });
  return resp.json();
}

async function loadFiles() {
  files = await api('list', {});
  const list = document.getElementById('file-list');
  files.sort(function(a, b) { return a.path.localeCompare(b.path); });
  list.innerHTML = '';
  for (const f of files) {
    const div = document.createElement('div');
    div.className = 'sidebar-item' + (f.path === currentPath ? ' active' : '');
    div.textContent = f.path;
    div.addEventListener('click', function() { openFile(f.path); });
    list.appendChild(div);
  }
}

async function openFile(path) {
  currentPath = path;
  const data = await api('get', { path: path });
  if (data.error) return;
  document.getElementById('current-file').textContent = path;
  document.getElementById('save-btn').disabled = false;
  document.getElementById('delete-btn').disabled = false;
  if (!editor) {
    editor = CodeMirror(document.getElementById('editor-wrap'), {
      value: data.content,
      mode: modeForPath(path),
      lineNumbers: true,
      indentUnit: 2,
      tabSize: 2,
      indentWithTabs: false,
      extraKeys: { 'Ctrl-S': saveFile, 'Cmd-S': saveFile }
    });
  } else {
    editor.setValue(data.content);
    editor.setOption('mode', modeForPath(path));
  }
  document.querySelectorAll('.sidebar-item').forEach(function(el) {
    el.className = 'sidebar-item' + (el.textContent === path ? ' active' : '');
  });
}

async function saveFile() {
  if (!currentPath || !editor) return;
  await apiPost('save', { path: currentPath, content: editor.getValue() });
  loadFiles();
}

async function deleteFile() {
  if (!currentPath) return;
  if (!confirm('Delete ' + currentPath + '?')) return;
  await apiPost('del', { path: currentPath });
  currentPath = null;
  document.getElementById('current-file').textContent = 'Select a file';
  document.getElementById('save-btn').disabled = true;
  document.getElementById('delete-btn').disabled = true;
  if (editor) editor.setValue('');
  loadFiles();
}

async function createFile() {
  const path = prompt('File path (e.g. hello/index.ts):');
  if (!path) return;
  await apiPost('save', { path: path, content: '' });
  await loadFiles();
  openFile(path);
}

loadFiles();
