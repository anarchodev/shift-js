var replayData = null;
var moduleSources = {};

async function loadReplay() {
  var resp = await fetch('/_api/logs?fn=getReplay&request_id=' + REQUEST_ID);
  replayData = await resp.json();
  if (replayData.error) {
    document.getElementById('request-info').innerHTML =
      '<div class="status-msg error">Replay data not found for this request.</div>';
    return;
  }

  // Show request info
  var rd = replayData.request_data;
  var infoEl = document.getElementById('request-info');
  infoEl.innerHTML = '';
  var table = document.createElement('table');
  if (rd) {
    addRow(table, 'Method', rd.method || '');
    addRow(table, 'Path', rd.path || '');
    if (rd.body) addRow(table, 'Body', rd.body);
  }
  var respd = replayData.response_data;
  if (respd) {
    addRow(table, 'Status', String(respd.status || ''));
  }
  infoEl.appendChild(table);

  // Load module sources
  var mt = replayData.module_tree || [];
  var modEl = document.getElementById('module-list');
  var modH = document.createElement('h3');
  modH.style.cssText = 'font-size:14px;margin-bottom:8px;color:#8b949e';
  modH.textContent = 'Modules (' + mt.length + ')';
  modEl.appendChild(modH);

  for (var i = 0; i < mt.length; i++) {
    var mod = mt[i];
    var modDiv = document.createElement('div');
    modDiv.style.cssText = 'font-family:monospace;font-size:13px;padding:4px 0;color:#58a6ff';
    modDiv.textContent = mod.path;
    var hashSpan = document.createElement('span');
    hashSpan.style.cssText = 'color:#484f58;margin-left:8px';
    hashSpan.textContent = (mod.content_hash || '').substring(0, 12) + '...';
    modDiv.appendChild(hashSpan);
    modEl.appendChild(modDiv);

    if (mod.content_hash) {
      try {
        var blobResp = await fetch('/_api/logs?fn=getBlob&hash=' + mod.content_hash);
        var blobData = await blobResp.json();
        if (blobData.content) {
          moduleSources[mod.path] = blobData.content;
        }
      } catch (e) { /* ignore */ }
    }
  }

  await registerModulesForDevtools();
  document.getElementById('replay-btn').disabled = false;
}

function addRow(table, label, value) {
  var tr = document.createElement('tr');
  var td1 = document.createElement('td');
  td1.textContent = label;
  var td2 = document.createElement('td');
  td2.textContent = value;
  tr.appendChild(td1);
  tr.appendChild(td2);
  table.appendChild(tr);
}

async function registerModulesForDevtools() {
  var mt = replayData.module_tree || [];

  // Fetch source maps from KV for TS/TSX modules
  var smByPath = {};
  for (var i = 0; i < mt.length; i++) {
    var mod = mt[i];
    if (!mod.path.endsWith('.ts') && !mod.path.endsWith('.tsx')) continue;
    try {
      var smResp = await fetch('/_api/logs?fn=getSourceMap&path=' + encodeURIComponent(mod.path));
      var smData = await smResp.json();
      if (smData.sourceMap) {
        smByPath[mod.path] = typeof smData.sourceMap === 'string'
          ? JSON.parse(smData.sourceMap) : smData.sourceMap;
      }
    } catch (e) { /* ignore */ }
  }

  for (var i = 0; i < mt.length; i++) {
    var mod = mt[i];
    var source = moduleSources[mod.path];
    if (!source) continue;

    var isTsx = mod.path.endsWith('.tsx');
    var isTs = mod.path.endsWith('.ts');
    var isEjs = mod.path.endsWith('.ejs');
    var needsTranspile = isTsx || isTs || isEjs;

    var jsSource;
    if (needsTranspile) {
      // Fetch the server-transpiled JS
      try {
        var trResp = await fetch('/_api/logs?fn=getTranspiled&path=' + encodeURIComponent(mod.path));
        var trData = await trResp.json();
        if (trData.content) {
          jsSource = trData.content;
        }
      } catch (e) { /* ignore */ }
    }

    // Fall back to raw source for .mjs files or if transpiled not available
    if (!jsSource) {
      jsSource = source;
    }

    jsSource = stripModuleSyntax(jsSource);
    moduleSources[mod.path + '__js'] = jsSource;

    // Attach source map if available (maps transpiled JS back to original TS/TSX)
    var smData = smByPath[mod.path];
    var scriptContent;
    if (smData) {
      var smObj = typeof smData === 'string' ? JSON.parse(smData) : smData;

      // Use absolute paths to avoid relative path resolution issues
      var absPath = '/' + mod.path;

      // Point sources to the original file and embed its content
      smObj.sources = [absPath];
      smObj.sourcesContent = [source];

      // For TSX, offset mappings by the prepended JSX runtime lines
      if (isTsx && smObj.mappings) {
        smObj.mappings = ';;' + smObj.mappings;
      }

      var smJson = JSON.stringify(smObj);
      var b64 = btoa(unescape(encodeURIComponent(smJson)));
      // sourceURL must differ from sources entry so devtools shows the original
      scriptContent = jsSource +
        '\n//# sourceMappingURL=data:application/json;base64,' + b64 +
        '\n//# sourceURL=' + absPath + '.compiled.js';
    } else {
      // No source map — just name the script directly
      scriptContent = jsSource + '\n//# sourceURL=/' + mod.path;
    }

    var script = document.createElement('script');
    script.textContent = scriptContent;
    document.head.appendChild(script);
  }
}

function stripModuleSyntax(source) {
  var lines = source.split('\n');
  var result = [];

  for (var li = 0; li < lines.length; li++) {
    var line = lines[li];

    // Remove import statements
    if (/^\s*import\s/.test(line)) {
      result.push('');
      continue;
    }

    // export default function name(  →  function name(
    // export default function(  →  function __default(
    var m = line.match(/^(\s*)export\s+default\s+function\s+(\w+)/);
    if (m) {
      line = m[1] + 'function ' + m[2] + line.slice(m[0].length);
      result.push(line);
      continue;
    }
    m = line.match(/^(\s*)export\s+default\s+function\s*(\()/);
    if (m) {
      line = m[1] + 'function __default' + m[2] + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export default <expr>  →  var __default = <expr>
    m = line.match(/^(\s*)export\s+default\s+/);
    if (m) {
      line = m[1] + 'var __default = ' + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export function name(  →  function name(
    m = line.match(/^(\s*)export\s+function\s+(\w+)/);
    if (m) {
      line = m[1] + 'function ' + m[2] + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export const/let/var  →  const/let/var
    m = line.match(/^(\s*)export\s+(const|let|var)\s/);
    if (m) {
      line = m[1] + m[2] + ' ' + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    result.push(line);
  }

  return result.join('\n');
}

function setupShims() {
  if (!replayData) return;

  var kvTape = replayData.kv_tape || [];
  var dateTape = replayData.date_tape || [];
  var mathRandomTape = replayData.math_random_tape || [];
  var rd = replayData.request_data || {};

  var kvIdx = 0;
  var dateIdx = 0;
  var mathIdx = 0;

  // Install shims as actual globals so the injected script's functions
  // (where the user sets breakpoints) reference them directly.
  window.kv = {
    get: function(key) {
      var entry = kvTape[kvIdx++];
      if (entry && entry.op === 'get') return entry.value;
      return null;
    },
    put: function(key, value) { kvIdx++; },
    delete: function(key) { kvIdx++; },
    range: function(start, end, count) {
      var entry = kvTape[kvIdx++];
      if (entry && entry.op === 'range') return entry.results || [];
      return [];
    }
  };

  window.request = {
    method: rd.method || 'GET',
    path: rd.path || '/',
    body: rd.body || null,
    headers: rd.headers || {},
    id: REQUEST_ID
  };

  window.response = {
    status: function(code) { console.log('[replay] response.status(' + code + ')'); },
    header: function(name, value) { console.log('[replay] response.header(' + name + ', ' + value + ')'); }
  };

  var sessionData = rd.session || {};
  window.session = {
    id: sessionData.id || '',
    get: function(key) { return sessionData[key] || null; },
    set: function(key, value) { sessionData[key] = value; },
    delete: function(key) { delete sessionData[key]; }
  };

  window.code = {
    get: function() { return null; },
    put: function() {},
    delete: function() {},
    list: function() { return []; }
  };

  window.logs = {
    query: function() { return []; },
    replay: function() { return null; }
  };

  // Override Date.now and Math.random
  window.__origDateNow = Date.now;
  Date.now = function() {
    return dateTape[dateIdx++] || window.__origDateNow();
  };
  window.__origMathRandom = Math.random;
  Math.random = function() {
    return mathRandomTape[mathIdx++] || 0;
  };
}

function escHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

async function doReplay() {
  if (!replayData) return;
  var output = document.getElementById('replay-output');
  var statusEl = document.getElementById('replay-status');

  // Set up globals so the injected script functions use replay tapes
  setupShims();

  // Parse the original request to find fn name and args
  var rd = replayData.request_data || {};
  var origPath = rd.path || '';
  var origQs = origPath.split('?')[1] || '';
  var origParams = {};
  origQs.split('&').forEach(function(pair) {
    var parts = pair.split('=');
    if (parts[0]) origParams[parts[0]] = decodeURIComponent(parts[1] || '');
  });

  var args = {};
  Object.keys(origParams).forEach(function(k) {
    if (k !== 'fn') args[k] = origParams[k];
  });

  var fnName = origParams.fn || 'default';
  if (rd.method === 'POST' && rd.body) {
    try {
      var body = JSON.parse(rd.body);
      if (body.fn) fnName = body.fn;
      if (body.args) Object.assign(args, body.args);
    } catch (e) { /* ignore */ }
  }

  // The functions were already defined globally by the injected <script> tags
  // in registerModulesForDevtools. Call them directly so breakpoints work.
  var fn = window[fnName];
  if (!fn) {
    // Try __default for default exports
    fn = window['__default'];
    if (!fn) {
      output.textContent = 'Function "' + fnName + '" not found. Available globals: ' +
        Object.keys(window).filter(function(k) { return typeof window[k] === 'function'; }).join(', ');
      return;
    }
  }

  try {
    var result = fn(args);
    var display = typeof result === 'object' ? JSON.stringify(result, null, 2) : String(result);
    output.textContent = display;
    statusEl.innerHTML = '<div class="status-msg ok">Replay completed successfully</div>';
  } catch (err) {
    output.textContent = 'Error: ' + err.message + '\n\n' + err.stack;
    statusEl.innerHTML = '<div class="status-msg error">Replay failed: ' + escHtml(err.message) + '</div>';
  } finally {
    // Restore Date.now and Math.random
    if (window.__origDateNow) Date.now = window.__origDateNow;
    if (window.__origMathRandom) Math.random = window.__origMathRandom;
  }
}

document.getElementById('replay-btn').addEventListener('click', doReplay);
loadReplay();
