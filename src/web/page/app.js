/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - the page: editor tabs, message pane, dialogs.
   Everything goes through the requests of src/web/api.c.  DARKSOFT. */
(function () {
"use strict";

var TOKEN = "";                 /* of this run of freecupl serve */
var MAC = /Mac|iPhone|iPad/.test(navigator.platform || "");
var COLOUR_LIMIT = 400000;      /* characters, as in freecuplw */
var LINE_PX = 18;               /* line-height of app.css */

function $(id) { return document.getElementById(id); }

var info = { version: "", dir: "", eol: "lf", skeleton: "" };
var docs = [];                  /* open tabs */
var active = -1;
var files = [];                 /* names in the directory */
var newCount = 0;
var lastTarget = "";
var running = false;
var prefs = { tab: 8, colour: true };
var findMode = "";              /* "", "find", "replace" */
var ended = false;

/* ---- requests ---- */

function api(method, path, body) {
  return fetch(path, {
    method: method,
    headers: { "X-FreeCUPL-Token": TOKEN },
    body: body,
    cache: "no-store",
    credentials: "omit"
  }).catch(function () {
    if (!ended) status("No answer from freecupl serve.");
    throw new Error("No answer from freecupl serve.");
  });
}

/* the text of a refusal: {"error": ...} or a plain line */
function failure(r) {
  return r.text().then(function (t) {
    var m = "";
    try { m = JSON.parse(t).error || ""; } catch (e) { m = t.trim().split("\n")[0]; }
    throw new Error(m || ("Error " + r.status));
  });
}

function apiJson(method, path, body) {
  return api(method, path, body).then(function (r) {
    return r.ok ? r.json() : failure(r);
  });
}

function q(s) { return encodeURIComponent(s); }

function status(text) { $("st-msg").textContent = text; }

/* ---- source text: the rules of gui_scan in src/gui/core.c ---- */

var KW = Object.create(null);
FC_KEYWORDS.forEach(function (k) { KW[k] = 1; });

function isId(c) {
  return (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57) || c === 95;
}

function isAlpha(c) {
  return (c >= 97 && c <= 122) || (c >= 65 && c <= 90);
}

/* class of every character */
function scan(t) {
  var n = t.length, a = new Uint8Array(n), i = 0, j, c, cc, num;
  while (i < n) {
    c = t.charCodeAt(i);
    if (c === 47 && i + 1 < n && t.charCodeAt(i + 1) === 42) {
      j = i + 2;
      while (j < n && !(t.charCodeAt(j) === 42 && j + 1 < n && t.charCodeAt(j + 1) === 47)) j++;
      j = j < n ? j + 2 : n;
      a.fill(FC_CLASS.comment, i, j);
      i = j;
    } else if (c === 39 && i + 2 < n && t.charCodeAt(i + 2) === 39 &&
               "bBoOdDhH".indexOf(t.charAt(i + 1)) >= 0) {
      /* 'h'1F */
      j = i + 3;
      while (j < n && isId(t.charCodeAt(j))) j++;
      a.fill(FC_CLASS.number, i, j);
      i = j;
    } else if (c === 36 && i + 1 < n && isAlpha(t.charCodeAt(i + 1))) {
      j = i + 1;
      while (j < n && isId(t.charCodeAt(j))) j++;
      a.fill(FC_CLASS.keyword, i, j);
      i = j;
    } else if (isId(c)) {
      num = true;
      j = i;
      while (j < n && isId(cc = t.charCodeAt(j))) {
        if (cc < 48 || cc > 57) num = false;
        j++;
      }
      a.fill(num ? FC_CLASS.number :
             KW[t.slice(i, j).toLowerCase()] ? FC_CLASS.keyword : FC_CLASS.plain, i, j);
      i = j;
    } else {
      i++;
    }
  }
  return a;
}

function esc(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

/* the text as HTML: runs of one class, the found text marked */
function render(t, attr, mark) {
  var out = [], i = 0, n = t.length, j, cls, cut;
  var m0 = mark ? mark[0] : -1, m1 = mark ? mark[1] : -1;
  while (i < n) {
    cls = attr ? attr[i] : 0;
    j = i + 1;
    cut = i < m0 ? m0 : i < m1 ? m1 : n;
    if (attr) while (j < cut && attr[j] === cls) j++;
    else j = cut;
    var s = esc(t.slice(i, j));
    if (cls) s = '<span class="c' + cls + '">' + s + "</span>";
    if (i >= m0 && i < m1) s = "<mark>" + s + "</mark>";
    out.push(s);
    i = j;
  }
  /* the last line of a textarea has height when it is empty */
  out.push("\n ");
  return out.join("");
}

/* ---- text and bytes ---- */

function decode(buf) {
  var bytes = new Uint8Array(buf), s, i, parts;
  try {
    s = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
    return { text: s, enc: "utf8" };
  } catch (e) {
    /* not UTF-8: one character for every byte */
    parts = [];
    for (i = 0; i < bytes.length; i += 8192)
      parts.push(String.fromCharCode.apply(null, bytes.subarray(i, i + 8192)));
    return { text: parts.join(""), enc: "bytes" };
  }
}

/* null when a character has no byte in a file read as bytes */
function encode(text, enc) {
  var i, c, out;
  if (enc === "utf8") return new TextEncoder().encode(text);
  out = new Uint8Array(text.length);
  for (i = 0; i < text.length; i++) {
    c = text.charCodeAt(i);
    if (c > 255) return null;
    out[i] = c;
  }
  return out;
}

function lineOf(t, pos) {
  var n = 1, i = -1;
  while ((i = t.indexOf("\n", i + 1)) >= 0 && i < pos) n++;
  return n;
}

function lineStart(t, line) {
  var i = 0, n = 1;
  while (n < line) {
    i = t.indexOf("\n", i);
    if (i < 0) return t.length;
    i++;
    n++;
  }
  return i;
}

function countLines(t) {
  var n = 1, i = -1;
  while ((i = t.indexOf("\n", i + 1)) >= 0) n++;
  return n;
}

/* ---- one tab ---- */

/* kind: "src" (edited), "view" (output file, read only), "guide" */
function Doc(kind, name, title) {
  var d = this;
  d.kind = kind;
  d.name = name;
  d.title = title || name;
  d.eol = info.eol;
  d.enc = "utf8";
  d.saved = "";
  d.mtime = "";
  d.modified = false;
  d.mark = null;
  d.nlines = 0;
  d.pending = false;

  d.el = document.createElement("div");
  d.el.className = "ed";
  d.gut = document.createElement("div");
  d.gut.className = "gut";
  d.gutPre = document.createElement("pre");
  d.gut.appendChild(d.gutPre);
  var code = document.createElement("div");
  code.className = "code";
  d.hl = document.createElement("pre");
  d.hl.className = "hl";
  d.ta = document.createElement("textarea");
  d.ta.spellcheck = false;
  d.ta.wrap = "off";
  d.ta.setAttribute("autocomplete", "off");
  d.ta.setAttribute("autocapitalize", "off");
  d.ta.setAttribute("autocorrect", "off");
  d.ta.readOnly = kind !== "src";
  code.appendChild(d.hl);
  code.appendChild(d.ta);
  d.el.appendChild(d.gut);
  d.el.appendChild(code);

  d.ta.addEventListener("input", function () {
    d.mark = null;
    d.modified = d.ta.value !== d.saved;
    d.paintSoon();
    tabsDraw();
    posDraw();
  });
  d.ta.addEventListener("scroll", function () { d.scrollSync(); });
  d.ta.addEventListener("keydown", function (e) {
    if (e.key === "Tab" && !e.shiftKey && !e.ctrlKey && !e.metaKey && !e.altKey &&
        !d.ta.readOnly) {
      e.preventDefault();
      insertText(d, "\t");
    }
  });
  ["keyup", "click", "select", "focus"].forEach(function (ev) {
    d.ta.addEventListener(ev, posDraw);
  });
}

Doc.prototype.setText = function (t) {
  this.ta.value = t;
  this.saved = t;
  this.modified = false;
  this.mark = null;
  this.paint();
};

Doc.prototype.scrollSync = function () {
  var x = -this.ta.scrollLeft, y = -this.ta.scrollTop;
  this.hl.style.transform = "translate(" + x + "px," + y + "px)";
  this.gutPre.style.transform = "translate(0," + y + "px)";
};

Doc.prototype.paintSoon = function () {
  var d = this;
  if (d.pending) return;
  d.pending = true;
  requestAnimationFrame(function () {
    d.pending = false;
    d.paint();
  });
};

Doc.prototype.paint = function () {
  var t = this.ta.value, attr = null, n, i, nums;
  if (this.kind === "src" && prefs.colour && t.length <= COLOUR_LIMIT) attr = scan(t);
  this.hl.innerHTML = render(t, attr, this.mark);
  n = countLines(t);
  if (n !== this.nlines) {
    nums = [];
    for (i = 1; i <= n; i++) nums.push(i);
    this.gutPre.textContent = nums.join("\n");
    this.nlines = n;
  }
  this.scrollSync();
};

/* ---- editing through the browser, so that its undo covers it ---- */

function insertText(d, text) {
  var ta = d.ta, ok = false, s = ta.selectionStart, e = ta.selectionEnd;
  ta.focus();
  try {
    ok = text === "" ? (s === e || document.execCommand("delete"))
                     : document.execCommand("insertText", false, text);
  } catch (err) {
    ok = false;
  }
  if (!ok) {
    ta.setRangeText(text, s, e, "end");
    ta.dispatchEvent(new Event("input", { bubbles: true }));
  }
}

function selectRange(d, a, b, focus) {
  var ta = d.ta, t = ta.value, line = lineOf(t, a), top, ls, col, x, cw;
  if (focus) ta.focus();
  ta.setSelectionRange(a, b);
  top = (line - 1) * LINE_PX;
  if (top < ta.scrollTop || top + 2 * LINE_PX > ta.scrollTop + ta.clientHeight)
    ta.scrollTop = Math.max(0, top - Math.floor(ta.clientHeight / 3));
  ls = t.lastIndexOf("\n", a - 1) + 1;
  col = t.slice(ls, a).replace(/\t/g, "        ".slice(0, prefs.tab)).length;
  cw = charWidth();
  x = col * cw;
  if (x < ta.scrollLeft || x + 20 * cw > ta.scrollLeft + ta.clientWidth)
    ta.scrollLeft = Math.max(0, x - Math.floor(ta.clientWidth / 2));
  d.scrollSync();
  posDraw();
}

var cwCache = 0;
function charWidth() {
  var s;
  if (cwCache) return cwCache;
  s = document.createElement("pre");
  s.className = "hl";
  s.style.visibility = "hidden";
  s.textContent = "MMMMMMMMMMMMMMMMMMMM";
  document.body.appendChild(s);
  cwCache = (s.getBoundingClientRect().width - 12) / 20 || 8;
  document.body.removeChild(s);
  return cwCache;
}

function gotoLine(d, line) {
  var t = d.ta.value, a, b;
  if (line < 1) line = 1;
  a = lineStart(t, line);
  b = t.indexOf("\n", a);
  if (b < 0) b = t.length;
  selectRange(d, a, b, true);
}

/* ---- tabs ---- */

function cur() { return active >= 0 ? docs[active] : null; }

function tabsDraw() {
  var box = $("tabs"), i;
  box.textContent = "";
  docs.forEach(function (d, k) {
    var t = document.createElement("div"), x = document.createElement("span");
    t.className = "tab" + (k === active ? " on" : "");
    t.textContent = d.title + (d.modified ? " *" : "");
    t.title = d.kind === "src" ? d.title : d.title + " (read only)";
    x.className = "x";
    x.textContent = "x";
    x.title = "Close";
    t.appendChild(x);
    t.addEventListener("mousedown", function (e) {
      if (e.target === x) return;
      activate(k);
    });
    x.addEventListener("click", function () { closeDoc(k); });
    box.appendChild(t);
  });
  $("empty").hidden = docs.length > 0;
  var names = Object.create(null);
  docs.forEach(function (d) { if (d.name) names[d.name] = 1; });
  var rows = $("files").children;
  for (i = 0; i < rows.length; i++)
    rows[i].classList.toggle("open", !!names[rows[i].dataset.name]);
  document.title = (cur() ? cur().title + " - " : "") + "FreeCUPL";
}

function posDraw() {
  var d = cur(), t, p, ls;
  if (!d) {
    $("st-pos").textContent = "";
    $("st-eol").textContent = "";
    return;
  }
  t = d.ta.value;
  p = d.ta.selectionStart;
  ls = t.lastIndexOf("\n", p - 1) + 1;
  $("st-pos").textContent = "Ln " + lineOf(t, p) + ", Col " + (p - ls + 1);
  $("st-eol").textContent = d.kind !== "src" ? "Read only" : d.eol === "crlf" ? "CRLF" : "LF";
}

function activate(k) {
  active = k;
  /* not display: none, which costs a textarea its undo history in Firefox */
  docs.forEach(function (d, i) { d.el.classList.toggle("off", i !== k); });
  tabsDraw();
  if (cur()) {
    cur().scrollSync();
    cur().ta.focus();
  }
  posDraw();
}

function addDoc(d) {
  $("eds").appendChild(d.el);
  docs.push(d);
  tabStyle(d);
  activate(docs.length - 1);
}

function tabStyle(d) {
  [d.hl, d.ta].forEach(function (e) {
    e.style.tabSize = prefs.tab;
    e.style.MozTabSize = prefs.tab;
  });
}

function findDoc(name) {
  for (var i = 0; i < docs.length; i++) if (docs[i].name === name) return i;
  return -1;
}

function closeDoc(k) {
  var d = docs[k];
  function drop() {
    k = docs.indexOf(d);
    if (k < 0) return;
    d.el.remove();
    docs.splice(k, 1);
    activate(docs.length ? Math.min(active > k ? active - 1 : active, docs.length - 1) : -1);
  }
  if (!d) return Promise.resolve(false);
  if (!d.modified) {
    drop();
    return Promise.resolve(true);
  }
  activate(k);
  return ask("Save the changes to " + d.title + "?", ["Save", "Discard", "Cancel"]).then(function (a) {
    if (a === "Discard") { drop(); return true; }
    if (a !== "Save") return false;
    return saveDoc(d, false).then(function (ok) {
      if (ok) drop();
      return ok;
    });
  });
}

/* ---- files ---- */

function filesLoad() {
  return apiJson("GET", "/api/files").then(function (j) {
    var box = $("files");
    files = j.files.map(function (f) { return f.name; });
    box.textContent = "";
    j.files.forEach(function (f) {
      var r = document.createElement("div");
      r.textContent = f.name;
      r.dataset.name = f.name;
      if (f.ro) r.className = "ro";
      r.addEventListener("click", function () { openFile(f.name, 0); });
      box.appendChild(r);
    });
    tabsDraw();
  });
}

function fetchFile(name) {
  return api("GET", "/api/file?name=" + q(name)).then(function (r) {
    if (!r.ok) return failure(r);
    return r.arrayBuffer().then(function (buf) {
      var x = decode(buf);
      return {
        text: x.text.replace(/\r\n?/g, "\n"),
        enc: x.enc,
        eol: r.headers.get("X-FreeCUPL-Eol") || "lf",
        mtime: r.headers.get("X-FreeCUPL-Mtime") || "",
        ro: r.headers.get("X-FreeCUPL-Readonly") === "1"
      };
    });
  });
}

var opening = Object.create(null);     /* files being read */

function openFile(name, line) {
  var k = findDoc(name);
  if (k >= 0) {
    activate(k);
    if (line > 0) gotoLine(docs[k], line);
    return Promise.resolve(true);
  }
  if (opening[name]) return opening[name];
  opening[name] = fetchFile(name).then(function (f) {
    var d = new Doc(f.ro ? "view" : "src", name);
    delete opening[name];
    d.eol = f.eol;
    d.enc = f.enc;
    d.mtime = f.mtime;
    addDoc(d);
    d.setText(f.text);
    d.ta.setSelectionRange(0, 0);
    if (line > 0) gotoLine(d, line);
    posDraw();
    status(name);
    return true;
  }).catch(function (e) {
    delete opening[name];
    status(e.message);
    return false;
  });
  return opening[name];
}

function newFile() {
  var d = new Doc("src", "", "new" + (++newCount) + ".pld");
  addDoc(d);
  d.setText(info.skeleton.replace(/\r\n/g, "\n"));
  d.saved = "";
  d.modified = true;
  tabsDraw();
  posDraw();
}

function askName(suggest) {
  var dlg = $("d-name"), inp = $("n-name");
  inp.value = suggest;
  $("n-err").textContent = "";
  return modal(dlg, inp, function () {
    var n = inp.value.trim();
    if (!/^[^.\s\/\\:*?"<>|][^\/\\:*?"<>|]*$/.test(n) || /[. ]$/.test(n)) {
      $("n-err").textContent = "Not a file name.";
      return false;
    }
    if (/\.(jed|doc|lst|so|wo|abs|mx)$/i.test(n)) {
      $("n-err").textContent = "That is the name of an output file.";
      return false;
    }
    return true;
  }).then(function (v) {
    return v === "ok" ? inp.value.trim() : "";
  });
}

function saveDoc(d, saveas) {
  var name = d.name, pick;
  if (d.kind !== "src") return Promise.resolve(false);
  if (saveas || !name) {
    pick = askName(name || d.title).then(function (n) {
      if (!n) return "";
      if (n !== d.name && files.indexOf(n) >= 0)
        return ask(n + " exists.  Replace it?", ["Replace", "Cancel"]).then(function (a) {
          return a === "Replace" ? n : "";
        });
      return n;
    });
  } else {
    pick = Promise.resolve(name);
  }
  return pick.then(function (n) {
    var text, bytes;
    if (!n) return false;
    text = d.ta.value;
    bytes = encode(d.eol === "crlf" ? text.replace(/\n/g, "\r\n") : text, d.enc);
    if (!bytes) {
      /* typed into a file that was not UTF-8 */
      return ask(d.title + " is not UTF-8 and now has characters outside its " +
                 "8-bit set.  Save it as UTF-8?", ["Save", "Cancel"]).then(function (a) {
        if (a !== "Save") return false;
        d.enc = "utf8";
        return saveDoc(d, false);
      });
    }
    return apiJson("POST", "/api/file?name=" + q(n), bytes).then(function (j) {
      d.name = d.title = n;
      d.saved = text;
      d.modified = d.ta.value !== text;
      d.mtime = j.mtime;
      status("Saved " + n);
      return filesLoad().then(function () { return true; });
    }).catch(function (e) {
      status(e.message);
      return ask(e.message, ["OK"]).then(function () { return false; });
    });
  });
}

/* open viewers follow the files */
function viewsReload() {
  return Promise.all(docs.filter(function (d) { return d.kind === "view"; }).map(function (d) {
    return apiJson("GET", "/api/file?stat=1&name=" + q(d.name)).then(function (j) {
      if (j.mtime === d.mtime) return null;
      return fetchFile(d.name).then(function (f) {
        var top = d.ta.scrollTop;
        d.mtime = f.mtime;
        d.setText(f.text);
        d.ta.scrollTop = top;
        d.scrollSync();
      });
    }).catch(function () { return null; });
  }));
}

function showGuide() {
  var k = findDoc("\u0001guide");
  if (k >= 0) { activate(k); return; }
  if (opening["\u0001guide"]) return;
  opening["\u0001guide"] = true;
  api("GET", "/api/guide").then(function (r) { return r.ok ? r.text() : failure(r); }).then(function (t) {
    var d = new Doc("guide", "\u0001guide", "User Guide");
    delete opening["\u0001guide"];
    addDoc(d);
    d.setText(t);
    d.ta.setSelectionRange(0, 0);
  }).catch(function (e) {
    delete opening["\u0001guide"];
    status(e.message);
  });
}

/* ---- compile and simulate ---- */

function target() {
  var d = cur();
  if (d && d.kind === "src" && d.name) return d.name;
  return lastTarget;
}

function uiDraw() {
  $("b-compile").disabled = $("b-sim").disabled = running;
  $("b-stop").disabled = !running;
}

function msgAdd(m) {
  var box = $("msgs"), r = document.createElement("div");
  r.className = "k-" + m.k;
  r.textContent = m.text;
  if (m.file && (m.line > 0 || m.k === "file")) {
    r.classList.add("go");
    r.dataset.file = m.file;
    r.dataset.line = m.line;
  }
  box.appendChild(r);
  box.scrollTop = box.scrollHeight;
}

function poll(n) {
  return apiJson("GET", "/api/job?from=" + n).then(function (j) {
    j.msgs.forEach(msgAdd);
    status(j.status);
    if (j.active)
      return new Promise(function (res) { setTimeout(res, 80); }).then(function () {
        return poll(j.total);
      });
    return null;
  });
}

/* shows the messages of the run on the server until it ends */
function follow() {
  running = true;
  uiDraw();
  $("msgs").textContent = "";
  return poll(0).then(function () {
    running = false;
    uiDraw();
    return filesLoad().then(viewsReload);
  }, function (e) {
    running = false;
    uiDraw();
    return ask(e.message, ["OK"]);
  });
}

var starting = false;           /* between the command and the run */

function run(action) {
  var d = cur(), first;
  if (running || starting) return Promise.resolve();
  starting = true;
  first = d && d.kind === "src" && !d.name ? saveDoc(d, true) : Promise.resolve(true);
  return first.then(function (ok) {
    var t = target(), chain = Promise.resolve(true);
    if (!ok) return null;
    if (!t) return ask("Open a .pld file first.", ["OK"]);
    docs.forEach(function (x) {
      if (x.kind === "src" && x.name && x.modified)
        chain = chain.then(function (good) { return good && saveDoc(x, false); });
    });
    return chain.then(function (good) {
      if (!good) return null;
      return apiJson("POST", "/api/run?action=" + action + "&name=" + q(t), "").then(function () {
        lastTarget = t;
        starting = false;
        return follow();
      }, function (e) {
        return ask(e.message, ["OK"]);
      });
    });
  }).then(function (v) {
    starting = false;
    return v;
  }, function (e) {
    starting = false;
    status(e.message);
  });
}

function viewOutput(key) {
  var t = target();
  if (!t) { status("Open a .pld file first."); return; }
  apiJson("GET", "/api/outputs?name=" + q(t)).then(function (j) {
    var name = j[key];
    if (key === "pld" || key === "si") {
      if (files.indexOf(name) < 0) { status("No file " + name); return; }
    } else if (!name) {
      status("No ." + key + " file for " + j.pld);
      return;
    }
    openFile(name, 0);
  }).catch(function (e) { status(e.message); });
}

/* ---- dialogs ---- */

/* Shows dlg; resolves with "ok", "cancel" or the label of the button.
   check() may keep the dialog open. */
function modal(dlg, focus, check) {
  return new Promise(function (res) {
    var form = dlg.querySelector("form");
    function done(v) {
      if (v === "ok" && check && !check()) return;
      form.removeEventListener("submit", onSubmit);
      dlg.removeEventListener("click", onClick);
      dlg.removeEventListener("close", onClose);
      if (dlg.open) dlg.close();
      if (cur()) cur().ta.focus();
      res(v);
    }
    function onSubmit(e) { e.preventDefault(); }
    function onClick(e) {
      var b = e.target.closest ? e.target.closest("button") : null;
      if (b && b.value) { e.preventDefault(); done(b.value); }
    }
    /* the event of an earlier close can arrive after the dialog is open
       again */
    function onClose() { if (!dlg.open) done("cancel"); }
    form.addEventListener("submit", onSubmit);
    dlg.addEventListener("click", onClick);
    dlg.addEventListener("close", onClose);
    dlg.onkeydown = function (e) {
      if (e.key === "Enter" && e.target.tagName === "INPUT" && e.target.type === "text") {
        e.preventDefault();
        done("ok");
      }
    };
    dlg.showModal();
    if (focus) { focus.focus(); if (focus.select) focus.select(); }
  });
}

function ask(text, buttons) {
  var box = $("a-btns");
  $("a-text").textContent = text;
  box.textContent = "";
  buttons.forEach(function (b) {
    var e = document.createElement("button");
    e.textContent = b;
    e.value = b;
    box.appendChild(e);
  });
  return modal($("d-ask"), box.firstChild, null);
}

var OPT_KEYS = ["jed", "doc", "fuseplot", "lst", "abs", "mx", "namesrc", "secure", "kb",
                "simafter", "compat", "simlatch", "simlst", "simjed"];
var devAll = null;

function devicesAll() {
  if (devAll) return Promise.resolve(devAll);
  return apiJson("GET", "/api/devices").then(function (j) {
    devAll = j.devs;
    return devAll;
  });
}

function options() {
  var t = target();
  return Promise.all([apiJson("GET", "/api/options" + (t ? "?name=" + q(t) : "")),
                      devicesAll()]).then(function (x) {
    var j = x[0], sel = $("o-device");
    if (sel.options.length === 1)
      x[1].forEach(function (d) {
        var o = document.createElement("option");
        o.value = o.textContent = d[0];
        sel.appendChild(o);
      });
    OPT_KEYS.forEach(function (k) { $("o-" + k).checked = !!j.opts[k]; });
    $("o-min").value = String(j.opts.min);
    sel.value = j.opts.device;
    $("o-perfile").checked = !!j.perfile;
    $("o-perfile").disabled = !j.source;
    $("o-title").textContent = j.source ? "Options - " + j.source : "Options";
    return modal($("d-options"), $("o-jed"), null).then(function (v) {
      var body;
      if (v !== "ok") return null;
      body = OPT_KEYS.map(function (k) { return k + "=" + ($("o-" + k).checked ? 1 : 0); });
      body.push("min=" + $("o-min").value);
      body.push("device=" + q(sel.value));
      return apiJson("POST", "/api/options?perfile=" + ($("o-perfile").checked ? 1 : 0) +
                     (j.source ? "&name=" + q(j.source) : ""), body.join("&"));
    });
  }).catch(function (e) { status(e.message); });
}

var devSel = "";
/* column the device list is sorted by (-1: order of the table), and
   the direction; the server sorts, with the rule freecuplw uses */
var devCol = -1, devDesc = false;

function devicesSort(col) {
  if (devCol === col) devDesc = !devDesc;
  else { devCol = col; devDesc = false; }
  return devicesFill();
}

function devicesFill() {
  var keep = devSel;
  return apiJson("GET", "/api/devices?q=" + q($("dv-filter").value) +
                 (devCol >= 0 ? "&s=" + devCol + "&d=" + (devDesc ? 1 : 0) : "")).then(function (j) {
    var box = $("dv-list"), found = false;
    [].forEach.call(document.querySelectorAll("#dv-table th"), function (th) {
      var on = Number(th.dataset.col) === devCol;
      th.className = on ? (devDesc ? "down" : "up") : "";
      if (on) th.setAttribute("aria-sort", devDesc ? "descending" : "ascending");
      else th.removeAttribute("aria-sort");
    });
    box.textContent = "";
    devSel = "";
    found = j.devs.some(function (d) { return d[0] === keep; });
    j.devs.forEach(function (d, i) {
      var tr = document.createElement("tr");
      d.forEach(function (v) {
        var td = document.createElement("td");
        td.textContent = v;
        tr.appendChild(td);
      });
      tr.dataset.mn = d[0];
      if (found ? d[0] === keep : i === 0) { tr.className = "sel"; devSel = d[0]; }
      box.appendChild(tr);
    });
    $("dv-count").textContent = j.devs.length + " of " + j.total;
    $("dv-insert").disabled = !devSel || !cur() || cur().kind !== "src";
  });
}

function devices() {
  var d = cur();
  $("dv-filter").value = "";
  return devicesFill().then(function () {
    return modal($("d-devices"), $("dv-filter"), function () { return !!devSel; });
  }).then(function (v) {
    if (v !== "ok" || !d || d.kind !== "src" || !devSel) return null;
    return insertDevice(d, devSel);
  }).catch(function (e) { status(e.message); });
}

/* Replace the Device statement, or put one on a new line at the caret. */
function insertDevice(d, mn) {
  var t = d.ta.value;
  /* one byte for every character, so that the offsets agree */
  var flat = t.replace(/[^\x00-\x7f]/g, "?");
  return apiJson("POST", "/api/devstmt?mn=" + q(mn), flat).then(function (j) {
    var p;
    if (j.found) {
      d.ta.setSelectionRange(j.start, j.end);
      insertText(d, j.text);
    } else {
      p = t.lastIndexOf("\n", d.ta.selectionStart - 1) + 1;
      d.ta.setSelectionRange(p, p);
      insertText(d, j.text + "\n");
    }
  });
}

function gotoDialog() {
  var d = cur(), inp = $("g-line");
  if (!d) return Promise.resolve();
  inp.value = String(lineOf(d.ta.value, d.ta.selectionStart));
  return modal($("d-goto"), inp, function () { return /^\s*\d+\s*$/.test(inp.value); }).then(function (v) {
    if (v === "ok") gotoLine(d, parseInt(inp.value, 10));
  });
}

function about() {
  $("ab-version").textContent = info.version;
  $("ab-dir").textContent = "Directory: " + info.dir;
  return modal($("d-about"), null, null);
}

/* ---- find and replace ---- */

function findShow(mode) {
  var d = cur(), sel;
  findMode = mode;
  $("find").hidden = false;
  $("f-rep").hidden = $("f-one").hidden = $("f-all").hidden = mode !== "replace";
  if (d) {
    sel = d.ta.value.slice(d.ta.selectionStart, d.ta.selectionEnd);
    if (sel && sel.indexOf("\n") < 0) $("f-what").value = sel;
  }
  $("f-what").focus();
  $("f-what").select();
}

function findHide() {
  var d = cur();
  findMode = "";
  $("find").hidden = true;
  if (d) {
    d.mark = null;
    d.paint();
    d.ta.focus();
  }
}

/* A-Z only: toLowerCase can change the length of a text */
function fold(s) {
  return $("f-case").checked ? s : s.replace(/[A-Z]+/g, function (m) { return m.toLowerCase(); });
}

/* dir 1 or -1; wraps round */
function findNext(dir) {
  var d = cur(), what = $("f-what").value, t, w, at;
  if (!d || !what) return false;
  t = fold(d.ta.value);
  w = fold(what);
  if (dir > 0) {
    at = t.indexOf(w, d.ta.selectionEnd);
    if (at < 0) at = t.indexOf(w);
  } else {
    at = d.ta.selectionStart - w.length < 0 ? -1 : t.lastIndexOf(w, d.ta.selectionStart - w.length);
    if (at < 0) at = t.lastIndexOf(w);
  }
  if (at < 0) {
    status("Not found: " + what);
    d.mark = null;
    d.paint();
    return false;
  }
  selectRange(d, at, at + w.length, false);
  /* the selection of a textarea without the focus is not drawn */
  d.mark = [at, at + w.length];
  d.paint();
  status("");
  return true;
}

function selMatches(d) {
  var what = $("f-what").value;
  return what && fold(d.ta.value.slice(d.ta.selectionStart, d.ta.selectionEnd)) === fold(what);
}

function replaceOne() {
  var d = cur(), back = document.activeElement;
  if (!d || d.kind !== "src") return;
  if (selMatches(d)) {
    insertText(d, $("f-with").value);
    if (back && back !== d.ta) back.focus();
  }
  findNext(1);
}

function replaceAll() {
  var d = cur(), what = $("f-what").value, t, f, w, out = [], i = 0, at, n = 0, back;
  if (!d || d.kind !== "src" || !what) return;
  t = d.ta.value;
  f = fold(t);
  w = fold(what);
  while ((at = f.indexOf(w, i)) >= 0) {
    out.push(t.slice(i, at), $("f-with").value);
    i = at + w.length;
    n++;
  }
  if (n) {
    out.push(t.slice(i));
    back = document.activeElement;
    /* one step for undo */
    d.ta.setSelectionRange(0, t.length);
    insertText(d, out.join(""));
    if (back && back !== d.ta) back.focus();
  }
  status(n + (n === 1 ? " replacement" : " replacements"));
}

/* ---- settings of the page ---- */

function prefsApply() {
  $("st-tab").value = String(prefs.tab);
  $("st-colour").checked = prefs.colour;
  docs.forEach(function (d) {
    tabStyle(d);
    d.paint();
  });
}

function prefsStore() {
  apiJson("POST", "/api/prefs?tab=" + prefs.tab + "&colour=" + (prefs.colour ? 1 : 0), "")
    .catch(function () {});
}

/* ---- end ---- */

function anyModified() {
  return docs.some(function (d) { return d.modified; });
}

function quit() {
  var go = anyModified() ? ask("There are changes not saved.  Quit and lose them?",
                               ["Quit", "Cancel"])
                         : Promise.resolve("Quit");
  return go.then(function (a) {
    if (a !== "Quit") return null;
    return apiJson("POST", "/api/quit", "").then(function () {
      var p = document.createElement("p");
      ended = true;
      docs = [];
      document.body.textContent = "";
      p.id = "ended";
      p.textContent = "freecupl serve has ended.  This tab can be closed.";
      document.body.appendChild(p);
    });
  });
}

/* ---- wiring ---- */

function on(id, fn) {
  $(id).addEventListener("click", function () { fn(); });
}

function keys(e) {
  var mod = MAC ? e.metaKey : e.ctrlKey, k = e.key, d = cur();
  var inDialog = !!document.querySelector("dialog[open]");
  if (ended || inDialog) return;
  function take() { e.preventDefault(); }
  if (mod && !e.altKey && (k === "s" || k === "S")) {
    take();
    if (d) saveDoc(d, false);
  } else if (mod && !e.altKey && !e.shiftKey && (k === "f" || k === "F")) {
    take();
    findShow("find");
  } else if ((!MAC && e.ctrlKey && (k === "h" || k === "H")) ||
             (MAC && e.metaKey && e.altKey && (k === "f" || k === "F" || e.code === "KeyF"))) {
    take();
    findShow("replace");
  } else if (e.ctrlKey && !e.altKey && (k === "g" || k === "G")) {
    take();
    gotoDialog();
  } else if (!MAC && e.ctrlKey && !e.altKey && (k === "d" || k === "D")) {
    take();
    devices();
  } else if (k === "F3") {
    take();
    if (!$("f-what").value) findShow("find");
    else findNext(e.shiftKey ? -1 : 1);
  } else if (k === "F7") {
    take();
    run("compile");
  } else if (k === "F8") {
    take();
    run("simulate");
  } else if (k === "F9") {
    take();
    options();
  } else if (k === "F1") {
    take();
    showGuide();
  } else if (k === "Escape" && findMode) {
    take();
    findHide();
  }
}

function start() {
  on("b-new", newFile);
  on("b-save", function () { if (cur()) saveDoc(cur(), false); });
  on("b-saveas", function () { if (cur()) saveDoc(cur(), true); });
  on("b-close", function () { if (cur()) closeDoc(active); });
  on("b-find", function () { findShow("find"); });
  on("b-replace", function () { findShow("replace"); });
  on("b-goto", gotoDialog);
  on("b-device", devices);
  on("b-compile", function () { run("compile"); });
  on("b-sim", function () { run("simulate"); });
  on("b-stop", function () { apiJson("POST", "/api/stop", "").catch(function () {}); });
  on("b-options", options);
  ["pld", "si", "jed", "doc", "lst", "so"].forEach(function (k) {
    on("b-v" + k, function () { viewOutput(k); });
  });
  on("b-guide", showGuide);
  on("b-about", about);
  on("b-quit", quit);
  on("b-refresh", filesLoad);
  on("f-next", function () { findNext(1); });
  on("f-prev", function () { findNext(-1); });
  on("f-one", replaceOne);
  on("f-all", replaceAll);
  on("f-close", findHide);
  $("f-what").addEventListener("keydown", function (e) {
    if (e.key === "Enter") { e.preventDefault(); findNext(e.shiftKey ? -1 : 1); }
  });
  $("f-with").addEventListener("keydown", function (e) {
    if (e.key === "Enter") { e.preventDefault(); replaceOne(); }
  });
  $("dv-filter").addEventListener("input", devicesFill);
  [].forEach.call(document.querySelectorAll("#dv-table th"), function (th) {
    th.addEventListener("click", function () { devicesSort(Number(th.dataset.col)); });
  });
  $("dv-list").addEventListener("click", function (e) {
    var tr = e.target.closest("tr"), old = $("dv-list").querySelector(".sel");
    if (!tr) return;
    if (old) old.className = "";
    tr.className = "sel";
    devSel = tr.dataset.mn;
  });
  $("dv-list").addEventListener("dblclick", function (e) {
    if (e.target.closest("tr") && !$("dv-insert").disabled) $("dv-insert").click();
  });
  $("msgs").addEventListener("click", function (e) {
    var r = e.target.closest(".go");
    if (r) openFile(r.dataset.file, parseInt(r.dataset.line, 10) || 0);
  });
  $("st-tab").addEventListener("change", function () {
    prefs.tab = parseInt($("st-tab").value, 10);
    prefsApply();
    prefsStore();
  });
  $("st-colour").addEventListener("change", function () {
    prefs.colour = $("st-colour").checked;
    prefsApply();
    prefsStore();
  });
  /* the bar between editor and messages */
  $("split").addEventListener("pointerdown", function (e) {
    var y0 = e.clientY, h0 = $("msgs").offsetHeight, bar = $("split");
    function move(ev) {
      var h = Math.max(40, Math.min(window.innerHeight - 200, h0 + y0 - ev.clientY));
      $("msgs").style.height = h + "px";
    }
    function up() {
      bar.removeEventListener("pointermove", move);
      bar.removeEventListener("pointerup", up);
    }
    bar.setPointerCapture(e.pointerId);
    bar.addEventListener("pointermove", move);
    bar.addEventListener("pointerup", up);
    e.preventDefault();
  });
  document.addEventListener("keydown", keys);
  window.addEventListener("beforeunload", function (e) {
    if (!ended && anyModified()) {
      e.preventDefault();
      e.returnValue = "";
    }
  });
  uiDraw();
}

/* the first requests, once the token is known */
function load() {
  apiJson("GET", "/api/info").then(function (j) {
    info = j;
    return apiJson("GET", "/api/options");
  }).then(function (j) {
    prefs.tab = j.tab === 2 || j.tab === 4 ? j.tab : 8;
    prefs.colour = !!j.colour;
    prefsApply();
    return filesLoad();
  }).then(function () {
    status(info.version + "  -  " + info.dir);
    return null;
  }).then(function () {
    /* the page was loaded again during a run */
    return apiJson("GET", "/api/job?from=0");
  }).then(function (j) {
    if (j.active) return follow();
    j.msgs.forEach(msgAdd);
    return null;
  }).catch(function (e) { status(e.message); });
}

/* ---- the token ---- */

/* kept for this tab, so that the page can be loaded again; nowhere else */
function tokenStored() {
  try { return sessionStorage.getItem("fc-token") || ""; } catch (e) { return ""; }
}

function tokenStore(t) {
  TOKEN = t;
  try { sessionStorage.setItem("fc-token", t); } catch (e) { /* this load only */ }
}

function noToken() {
  status("No access.  Open the address freecupl serve printed.");
}

/* The address ends in #t=TOKEN (printed by the program) or #k=TICKET
   (the browser the program opened; traded for the token, once). */
function boot() {
  var h = location.hash.slice(1), m;
  if (h) {
    try { history.replaceState(null, "", location.pathname); } catch (e) { location.hash = ""; }
  }
  start();
  if ((m = /^t=([0-9a-f]{32})$/.exec(h))) {
    tokenStore(m[1]);
    load();
  } else if ((m = /^k=([0-9a-f]{32})$/.exec(h))) {
    fetch("/api/session?k=" + m[1], { method: "POST", body: "", cache: "no-store", credentials: "omit" })
      .then(function (r) { return r.ok ? r.json() : failure(r); })
      .then(function (j) {
        tokenStore(j.token);
        load();
      }, function () {
        /* used before: this tab may have the token already */
        TOKEN = tokenStored();
        if (TOKEN) load();
        else noToken();
      });
  } else {
    TOKEN = tokenStored();
    if (TOKEN) load();
    else noToken();
  }
}

/* the printed address put into a tab that is already open */
window.addEventListener("hashchange", function () {
  if (/^#[tk]=/.test(location.hash)) location.reload();
});

/* for the tests */
window.fcScan = function (t) { return Array.prototype.join.call(scan(t), ""); };

boot();
})();
