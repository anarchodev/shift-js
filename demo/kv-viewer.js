let currentKey = null;

async function api(fn, args) {
  const resp = await fetch('/_api/kv?fn=' + fn + '&' + new URLSearchParams(args));
  return resp.json();
}

function escHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

async function loadKeys() {
  const prefix = document.getElementById('prefix-input').value;
  const keys = await api('list', { prefix: prefix, count: '500' });
  const list = document.getElementById('key-list');
  list.innerHTML = '';
  for (const k of keys) {
    const div = document.createElement('div');
    div.className = 'sidebar-item' + (k.key === currentKey ? ' active' : '');
    div.style.display = 'flex';
    div.style.alignItems = 'center';

    const nameSpan = document.createElement('span');
    nameSpan.textContent = k.key;
    const sizeSpan = document.createElement('span');
    sizeSpan.style.cssText = 'color:#484f58;font-size:11px;margin-left:auto;padding-left:8px';
    sizeSpan.textContent = k.size + 'B';

    div.appendChild(nameSpan);
    div.appendChild(sizeSpan);
    div.addEventListener('click', function() { openKey(k.key); });
    list.appendChild(div);
  }
}

async function openKey(key) {
  currentKey = key;
  document.getElementById('current-key').textContent = key;
  document.getElementById('save-btn').style.display = '';
  const data = await api('get', { key: key });
  const ta = document.getElementById('value-display');
  if (data.error) {
    ta.value = 'Error: ' + data.error;
    return;
  }
  try {
    const parsed = JSON.parse(data.value);
    ta.value = JSON.stringify(parsed, null, 2);
  } catch (e) {
    ta.value = data.value;
  }
  document.querySelectorAll('#key-list .sidebar-item').forEach(function(el) {
    const span = el.querySelector('span');
    el.className = 'sidebar-item' + (span && span.textContent === key ? ' active' : '');
  });
}

async function saveKey() {
  if (!currentKey) return;
  const value = document.getElementById('value-display').value;
  const resp = await fetch('/_api/kv?fn=put&key=' + encodeURIComponent(currentKey) + '&value=' + encodeURIComponent(value));
  const data = await resp.json();
  if (data.ok) {
    loadKeys();
  }
}

document.getElementById('prefix-input').addEventListener('keydown', function(e) {
  if (e.key === 'Enter') loadKeys();
});

loadKeys();
