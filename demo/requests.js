async function loadRequests() {
  var resp = await fetch('/_api/logs?fn=listRequests&limit=200');
  var entries = await resp.json();
  var container = document.getElementById('request-list');
  container.innerHTML = '';

  if (!entries || entries.length === 0) {
    container.innerHTML = '<div style="padding:20px;color:#8b949e;text-align:center">No requests yet. Make some requests first.</div>';
    return;
  }

  entries.forEach(function(e) {
    var req = e.request || {};
    var res = e.response || {};
    var div = document.createElement('div');
    div.className = 'log-row';
    var sc = (res.status || 200) >= 400 ? '#f85149' : '#3fb950';

    var method = document.createElement('span');
    method.className = 'method';
    method.style.color = sc;
    method.textContent = req.method || '?';

    var status = document.createElement('span');
    status.style.color = sc;
    status.style.width = '35px';
    status.textContent = res.status || '?';

    var path = document.createElement('span');
    path.className = 'path';
    path.textContent = req.path || '';

    var rid = document.createElement('span');
    rid.className = 'rid';
    rid.textContent = e.request_id;

    var btns = document.createElement('span');
    btns.style.cssText = 'display:flex;gap:6px;margin-left:8px';

    var logBtn = document.createElement('button');
    logBtn.className = 'btn';
    logBtn.textContent = 'Logs';
    logBtn.addEventListener('click', function(ev) {
      ev.stopPropagation();
      showLogs(e.request_id);
    });

    var repBtn = document.createElement('button');
    repBtn.className = 'btn btn-primary';
    repBtn.textContent = 'Replay';
    repBtn.addEventListener('click', function(ev) {
      ev.stopPropagation();
      location.href = '/replay?request_id=' + e.request_id;
    });

    btns.appendChild(logBtn);
    btns.appendChild(repBtn);
    div.appendChild(method);
    div.appendChild(status);
    div.appendChild(path);
    div.appendChild(rid);
    div.appendChild(btns);
    container.appendChild(div);
  });
}

function showLogs(requestId) {
  fetch('/_api/logs?fn=getLogs&request_id=' + requestId)
    .then(function(r) { return r.json(); })
    .then(function(entries) {
      var root = document.getElementById('modal-root');
      var overlay = document.createElement('div');
      overlay.style.cssText = 'position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.7);z-index:100;display:flex;align-items:center;justify-content:center';
      overlay.addEventListener('click', function(ev) {
        if (ev.target === overlay) overlay.remove();
      });

      var box = document.createElement('div');
      box.style.cssText = 'background:#161b22;border:1px solid #30363d;border-radius:8px;padding:20px;max-width:700px;width:90%;max-height:80vh;overflow:auto';

      var h = document.createElement('h3');
      h.style.cssText = 'margin-bottom:12px;font-size:14px';
      h.textContent = 'Logs for request ' + requestId;
      box.appendChild(h);

      if (!entries || entries.length === 0) {
        var p = document.createElement('p');
        p.style.cssText = 'color:#8b949e;font-size:13px';
        p.textContent = 'No console log entries for this request.';
        box.appendChild(p);
      } else {
        entries.forEach(function(e) {
          var d = document.createElement('div');
          d.style.cssText = 'font-family:monospace;font-size:13px;padding:4px 0;border-bottom:1px solid #21262d';
          var color = e.level === 'error' ? '#f85149' : e.level === 'warn' ? '#d29922' : '#8b949e';
          var lvl = document.createElement('span');
          lvl.style.cssText = 'color:' + color + ';width:40px;display:inline-block';
          lvl.textContent = e.level;
          d.appendChild(lvl);
          d.appendChild(document.createTextNode(' ' + e.message));
          box.appendChild(d);
        });
      }

      var closeBtn = document.createElement('button');
      closeBtn.className = 'btn';
      closeBtn.textContent = 'Close';
      closeBtn.style.marginTop = '12px';
      closeBtn.addEventListener('click', function() { overlay.remove(); });
      box.appendChild(closeBtn);

      overlay.appendChild(box);
      root.appendChild(overlay);
    });
}

document.getElementById('refresh-btn').addEventListener('click', loadRequests);
loadRequests();
