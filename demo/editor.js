let editor = null;
let currentPath = null;
let files = [];
let expandedDirs = {};

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

function buildTree(files) {
  var root = { children: {}, files: [] };
  for (var i = 0; i < files.length; i++) {
    var parts = files[i].path.split('/');
    var node = root;
    for (var j = 0; j < parts.length - 1; j++) {
      if (!node.children[parts[j]]) {
        node.children[parts[j]] = { children: {}, files: [] };
      }
      node = node.children[parts[j]];
    }
    node.files.push(files[i]);
  }
  return root;
}

function renderTree(container, node, prefix) {
  // Sort: directories first, then files
  var dirNames = Object.keys(node.children).sort();
  var sortedFiles = node.files.slice().sort(function(a, b) {
    var nameA = a.path.split('/').pop();
    var nameB = b.path.split('/').pop();
    return nameA.localeCompare(nameB);
  });

  for (var di = 0; di < dirNames.length; di++) {
    var dirName = dirNames[di];
    var dirPath = prefix ? prefix + '/' + dirName : dirName;
    var expanded = expandedDirs[dirPath] !== false; // default expanded

    var dirEl = document.createElement('div');
    dirEl.className = 'tree-dir';
    dirEl.setAttribute('data-path', dirPath);

    var label = document.createElement('div');
    label.className = 'tree-label tree-dir-label';
    label.style.paddingLeft = (countDepth(dirPath) * 16 + 8) + 'px';

    var arrow = document.createElement('span');
    arrow.className = 'tree-arrow';
    arrow.textContent = expanded ? '▾' : '▸';

    var icon = document.createElement('span');
    icon.className = 'tree-icon';
    icon.textContent = '📁';

    var name = document.createElement('span');
    name.textContent = dirName;

    label.appendChild(arrow);
    label.appendChild(icon);
    label.appendChild(name);
    dirEl.appendChild(label);

    var childContainer = document.createElement('div');
    childContainer.className = 'tree-children';
    childContainer.style.display = expanded ? 'block' : 'none';
    dirEl.appendChild(childContainer);

    (function(dp, cc, ar) {
      label.addEventListener('click', function() {
        var isExpanded = cc.style.display !== 'none';
        cc.style.display = isExpanded ? 'none' : 'block';
        ar.textContent = isExpanded ? '▸' : '▾';
        expandedDirs[dp] = !isExpanded;
      });
    })(dirPath, childContainer, arrow);

    container.appendChild(dirEl);
    renderTree(childContainer, node.children[dirName], dirPath);
  }

  for (var fi = 0; fi < sortedFiles.length; fi++) {
    var f = sortedFiles[fi];
    var fileName = f.path.split('/').pop();
    var fileEl = document.createElement('div');
    fileEl.className = 'tree-label tree-file' + (f.path === currentPath ? ' active' : '');
    fileEl.style.paddingLeft = (countDepth(f.path) * 16 + 8) + 'px';
    fileEl.setAttribute('data-path', f.path);

    var ficon = document.createElement('span');
    ficon.className = 'tree-icon';
    ficon.textContent = fileIcon(fileName);

    var fname = document.createElement('span');
    fname.textContent = fileName;

    fileEl.appendChild(ficon);
    fileEl.appendChild(fname);

    (function(path) {
      fileEl.addEventListener('click', function() { openFile(path); });
    })(f.path);

    container.appendChild(fileEl);
  }
}

function countDepth(path) {
  var count = 0;
  for (var i = 0; i < path.length; i++) {
    if (path[i] === '/') count++;
  }
  return count;
}

function fileIcon(name) {
  if (name.endsWith('.tsx') || name.endsWith('.jsx')) return '⚛';
  if (name.endsWith('.ts')) return '🇹';
  if (name.endsWith('.mjs') || name.endsWith('.js')) return '📜';
  if (name.endsWith('.css')) return '🎨';
  if (name.endsWith('.ejs') || name.endsWith('.html')) return '🌐';
  if (name.endsWith('.json')) return '{}';
  return '📄';
}

async function loadFiles() {
  files = await api('list', {});
  var list = document.getElementById('file-list');
  files.sort(function(a, b) { return a.path.localeCompare(b.path); });
  list.innerHTML = '';
  var tree = buildTree(files);
  renderTree(list, tree, '');
}

async function openFile(path) {
  currentPath = path;
  var data = await api('get', { path: path });
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
  // Update active state
  document.querySelectorAll('.tree-file').forEach(function(el) {
    el.className = 'tree-label tree-file' + (el.getAttribute('data-path') === path ? ' active' : '');
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
  var path = prompt('File path (e.g. hello/index.ts):');
  if (!path) return;
  await apiPost('save', { path: path, content: '' });
  await loadFiles();
  openFile(path);
}

loadFiles();
