var replayData = null;
var moduleSources = {};

// Module registry: __sjs_mod['path'] = { default: ..., namedExport: ..., ... }
window.__sjs_mod = {};

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

// Resolve an import specifier relative to the importing module's path.
// './bar' in 'foo/index.ts' → 'foo/bar'
// '../utils' in 'foo/bar/index.ts' → 'foo/utils'
function resolveImportPath(specifier, importerPath) {
  if (specifier[0] !== '.') return specifier; // bare specifier, return as-is

  var importerDir = importerPath.replace(/\/[^/]*$/, '');
  var parts = (importerDir + '/' + specifier).split('/');
  var resolved = [];
  for (var i = 0; i < parts.length; i++) {
    if (parts[i] === '.' || parts[i] === '') continue;
    if (parts[i] === '..') { resolved.pop(); continue; }
    resolved.push(parts[i]);
  }
  return resolved.join('/');
}

// Find which module_tree entry matches a resolved import path.
// The server tries: path.mjs, path.ts, path.tsx, path/index.mjs, path/index.ts, etc.
function findModulePath(resolvedBase, moduleTree) {
  var candidates = [
    resolvedBase,
    resolvedBase + '.mjs',
    resolvedBase + '.ts',
    resolvedBase + '.tsx',
    resolvedBase + '/index.mjs',
    resolvedBase + '/index.ts',
    resolvedBase + '/index.tsx'
  ];
  for (var ci = 0; ci < candidates.length; ci++) {
    for (var mi = 0; mi < moduleTree.length; mi++) {
      if (moduleTree[mi].path === candidates[ci]) return candidates[ci];
    }
  }
  return null;
}

// Process a module's source: rewrite imports and exports for browser execution.
// Returns { code: string, exportNames: string[], hasDefault: boolean }
function processModule(jsSource, modPath, moduleTree) {
  var lines = jsSource.split('\n');
  var result = [];
  var exportNames = [];
  var hasDefault = false;
  var defaultExportName = null;

  for (var li = 0; li < lines.length; li++) {
    var line = lines[li];
    var m;

    // import { a, b } from './path'
    // import { a as b } from './path'
    m = line.match(/^\s*import\s+\{([^}]+)\}\s+from\s+['"]([^'"]+)['"]\s*;?\s*$/);
    if (m) {
      var imports = m[1];
      var specifier = m[2];
      var resolved = findModulePath(resolveImportPath(specifier, modPath), moduleTree);
      if (resolved) {
        // Rewrite to destructure from module registry
        // Handle 'as' renames: { foo as bar } → get foo, assign to bar
        var importParts = imports.split(',');
        var assignments = [];
        for (var pi = 0; pi < importParts.length; pi++) {
          var part = importParts[pi].trim();
          var asMatch = part.match(/^(\w+)\s+as\s+(\w+)$/);
          if (asMatch) {
            assignments.push('var ' + asMatch[2] + ' = __sjs_mod[' + JSON.stringify(resolved) + '].' + asMatch[1]);
          } else {
            assignments.push('var ' + part + ' = __sjs_mod[' + JSON.stringify(resolved) + '].' + part);
          }
        }
        result.push(assignments.join('; '));
      } else {
        result.push(''); // can't resolve, blank the line
      }
      continue;
    }

    // import foo from './path' (default import)
    m = line.match(/^\s*import\s+(\w+)\s+from\s+['"]([^'"]+)['"]\s*;?\s*$/);
    if (m) {
      var name = m[1];
      var specifier = m[2];
      var resolved = findModulePath(resolveImportPath(specifier, modPath), moduleTree);
      if (resolved) {
        result.push('var ' + name + ' = __sjs_mod[' + JSON.stringify(resolved) + '].default');
      } else {
        result.push('');
      }
      continue;
    }

    // import * as foo from './path'
    m = line.match(/^\s*import\s+\*\s+as\s+(\w+)\s+from\s+['"]([^'"]+)['"]\s*;?\s*$/);
    if (m) {
      var name = m[1];
      var specifier = m[2];
      var resolved = findModulePath(resolveImportPath(specifier, modPath), moduleTree);
      if (resolved) {
        result.push('var ' + name + ' = __sjs_mod[' + JSON.stringify(resolved) + ']');
      } else {
        result.push('');
      }
      continue;
    }

    // import './path' (side-effect only)
    m = line.match(/^\s*import\s+['"]([^'"]+)['"]\s*;?\s*$/);
    if (m) {
      result.push(''); // already loaded via script tag
      continue;
    }

    // export default function name(
    m = line.match(/^(\s*)export\s+default\s+function\s+(\w+)/);
    if (m) {
      hasDefault = true;
      defaultExportName = m[2];
      exportNames.push(m[2]);
      line = m[1] + 'function ' + m[2] + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export default function(
    m = line.match(/^(\s*)export\s+default\s+function\s*(\()/);
    if (m) {
      hasDefault = true;
      line = m[1] + 'function __default' + m[2] + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export default <expr>
    m = line.match(/^(\s*)export\s+default\s+/);
    if (m) {
      hasDefault = true;
      line = m[1] + 'var __default = ' + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export function name(
    m = line.match(/^(\s*)export\s+function\s+(\w+)/);
    if (m) {
      exportNames.push(m[2]);
      line = m[1] + 'function ' + m[2] + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    // export const/let/var name
    m = line.match(/^(\s*)export\s+(const|let|var)\s+(\w+)/);
    if (m) {
      exportNames.push(m[3]);
      line = m[1] + m[2] + ' ' + m[3] + line.slice(m[0].length);
      result.push(line);
      continue;
    }

    result.push(line);
  }

  // Append module registry assignment
  var regParts = [];
  if (hasDefault) {
    regParts.push('default: typeof __default !== "undefined" ? __default : ' +
      (defaultExportName || 'undefined'));
  }
  for (var ei = 0; ei < exportNames.length; ei++) {
    regParts.push(exportNames[ei] + ': ' + exportNames[ei]);
  }

  result.push('__sjs_mod[' + JSON.stringify(modPath) + '] = {' + regParts.join(', ') + '};');

  return {
    code: result.join('\n'),
    exportNames: exportNames,
    hasDefault: hasDefault
  };
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

  // Inject setup script for module registry
  var setupScript = document.createElement('script');
  setupScript.textContent = 'window.__sjs_mod = {};';
  document.head.appendChild(setupScript);

  // Phase 1: Process all modules (fetch transpiled code, rewrite imports/exports)
  var scriptContents = [];
  for (var i = 0; i < mt.length; i++) {
    var mod = mt[i];
    var source = moduleSources[mod.path];
    if (!source) { scriptContents.push(null); continue; }

    var isTsx = mod.path.endsWith('.tsx');
    var isTs = mod.path.endsWith('.ts');
    var isEjs = mod.path.endsWith('.ejs');
    var needsTranspile = isTsx || isTs || isEjs;

    var jsSource;
    if (needsTranspile) {
      try {
        var trResp = await fetch('/_api/logs?fn=getTranspiled&path=' + encodeURIComponent(mod.path));
        var trData = await trResp.json();
        if (trData.content) {
          jsSource = trData.content;
        }
      } catch (e) { /* ignore */ }
    }

    if (!jsSource) {
      jsSource = source;
    }

    var processed = processModule(jsSource, mod.path, mt);
    moduleSources[mod.path + '__js'] = processed.code;

    var smData = smByPath[mod.path];
    var scriptContent;
    if (smData) {
      var smObj = typeof smData === 'string' ? JSON.parse(smData) : smData;
      var absPath = '/' + mod.path;
      smObj.sources = [absPath];
      smObj.sourcesContent = [source];
      var smJson = JSON.stringify(smObj);
      var b64 = btoa(unescape(encodeURIComponent(smJson)));
      scriptContent = processed.code +
        '\n//# sourceMappingURL=data:application/json;base64,' + b64 +
        '\n//# sourceURL=' + absPath + '.compiled.js';
    } else {
      scriptContent = processed.code + '\n//# sourceURL=/' + mod.path;
    }

    scriptContents.push(scriptContent);
  }

  // Phase 2: Inject scripts in reverse order (dependencies before importers).
  // module_tree records entry point first, then dependencies as they're loaded,
  // so reversing gives us leaf-dependencies-first order.
  for (var i = scriptContents.length - 1; i >= 0; i--) {
    if (!scriptContents[i]) continue;
    var script = document.createElement('script');
    script.textContent = scriptContents[i];
    document.head.appendChild(script);
  }
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

  setupShims();

  var mt = replayData.module_tree || [];
  if (mt.length === 0) {
    output.textContent = 'No modules in replay data';
    return;
  }

  // The entry point is the first module in the tree
  var mainMod = mt[0];
  var mainReg = window.__sjs_mod[mainMod.path];

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

  // Look up the function: first in the module registry, then as a global
  var fn = null;
  if (mainReg) {
    fn = mainReg[fnName] || mainReg['default'];
  }
  if (!fn) {
    fn = window[fnName] || window['__default'];
  }

  if (!fn || typeof fn !== 'function') {
    output.textContent = 'Function "' + fnName + '" not found.\nModule exports: ' +
      (mainReg ? Object.keys(mainReg).join(', ') : 'none');
    return;
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
    if (window.__origDateNow) Date.now = window.__origDateNow;
    if (window.__origMathRandom) Math.random = window.__origMathRandom;
  }
}

document.getElementById('replay-btn').addEventListener('click', doReplay);
loadReplay();
