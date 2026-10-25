// Sora's window. Talks to Sora.exe through chrome.webview: sends {cmd}, receives {type}.
"use strict";

const host = window.chrome && window.chrome.webview;
const $ = (s) => document.querySelector(s);
const send = (m) => host && host.postMessage(m);

const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* not saved */ } },
};

let songs = [];          // see addSong()
let selected = null;     // song id
let outDir = store.get("outDir", "");
let outDirAsked = false; // the folder is confirmed once per session, right before making
let busy = false;
let outputs = store.get("outputs", ["youtube_full", "ipod_short", "lyrics"]);
let nextId = 1;

const LABEL = { local: "Local", web: "Web", both: "Both", mine: "My own" };

// ---------------------------------------------------------------- songs

function addSong(path) {
  if (songs.some((s) => s.path.toLowerCase() === path.toLowerCase())) return;
  const name = path.split(/[\\/]/).pop().replace(/\.[^.]+$/, "");
  const id = "s" + Date.now().toString(36) + (nextId++);
  const s = {
    id, path, name,
    file: null,                           // tags as read from the file
    mode: "keep",                         // keep the file's tags | own
    own: { title: "", artist: "", album: "" },
    cover: null, coverUrl: null,          // file's embedded cover (or one the user picked)
    lyr: { state: "idle", summary: null, texts: {}, choice: null, mine: "", mineFile: null, searchedFor: "" },
    accent: "#ff6b4a",
    status: "reading", pct: 0, stage: "", files: [], error: "",
  };
  songs.push(s);
  send({ cmd: "tags", id, path });
  if (!selected) selected = id;
}

const song = (id) => songs.find((s) => s.id === id);

function tagsOf(s) {
  if (s.mode === "own") return s.own;
  const f = s.file || {};
  return { title: f.title || f.guess_title || s.name, artist: f.artist || "", album: f.album || "" };
}

function lyricsFile(s) {
  const c = s.lyr.choice;
  if (!c) return null;
  if (c === "mine") return s.lyr.mineFile;
  return s.lyr.summary && s.lyr.summary[c] && s.lyr.summary[c].file;
}

function searchLyrics(s) {
  const t = tagsOf(s);
  s.lyr.state = "loading";
  s.lyr.searchedFor = t.title + "|" + t.artist;
  send({ cmd: "lyrics", id: s.id, path: s.path, title: t.title, artist: t.artist });
}

function statusText(s) {
  if (s.status === "reading") return ["Reading…", ""];
  if (s.status === "queued") return ["Waiting", ""];
  if (s.status === "working") return [Math.round(s.pct * 100) + "%", ""];
  if (s.status === "done") return ["Done", "ok"];
  if (s.status === "failed") return ["Failed", "bad"];
  if (s.lyr.state === "loading") return ["Lyrics…", ""];
  if (!lyricsFile(s)) return ["No lyrics", "bad"];
  return ["Ready", ""];
}

// ---------------------------------------------------------------- render

function renderList() {
  const list = $("#list");
  list.innerHTML = "";
  for (const s of songs) {
    const t = tagsOf(s);
    const li = document.createElement("li");
    li.className = s.id === selected ? "sel" : "";
    const [st, cls] = statusText(s);
    li.innerHTML = `<div class="t"></div><div class="st ${cls}"></div><div class="a"></div>` +
      (s.status === "working" ? `<div class="bar"><i style="width:${(s.pct * 100).toFixed(1)}%"></i></div>` : "");
    li.querySelector(".t").textContent = t.title || s.name;
    li.querySelector(".a").textContent = s.status === "working" ? s.stage : (t.artist || s.path.split(/[\\/]/).pop());
    li.querySelector(".st").textContent = st;
    li.onclick = () => { selected = s.id; render(); };
    list.appendChild(li);
  }
  $("#drop-empty .dz-title").textContent = songs.length ? "Drop more songs here" : "Drop songs here";
  const ready = songs.filter((s) => lyricsFile(s) && s.status !== "done").length;
  $("#start").textContent = busy ? "Making…" : ready > 1 ? `Make ${ready} songs` : "Make";
  $("#start").disabled = busy || !ready || !outputs.length;
  $("#cancel").hidden = !busy;
}

function renderDetail() {
  const s = song(selected);
  $("#nothing").hidden = !!s;
  $("#detail").hidden = !s;
  if (!s) return;
  const t = tagsOf(s);
  $("#d-title").textContent = t.title || s.name;
  $("#d-sub").textContent = [t.artist, s.path].filter(Boolean).join("  ·  ");
  $("#d-cover").hidden = !s.coverUrl;
  if (s.coverUrl) $("#d-cover").src = s.coverUrl;

  // tags
  for (const r of document.querySelectorAll("input[name=tagmode]")) r.checked = r.value === s.mode;
  const own = s.mode === "own";
  for (const k of ["title", "artist", "album"]) {
    const el = $("#t-" + k);
    if (document.activeElement !== el) el.value = t[k] || "";
    el.disabled = !own;
  }
  $("#t-cover").hidden = !s.coverUrl;
  if (s.coverUrl) $("#t-cover").src = s.coverUrl;
  $("#t-nocover").hidden = !!s.coverUrl;
  $("#t-pickcover").hidden = !own;
  $("#t-clearcover").hidden = !own || !s.coverUrl;

  // lyrics
  const L = s.lyr, sum = L.summary;
  const status = $("#l-status");
  const searched = (t.title + "|" + t.artist) !== L.searchedFor && L.state !== "idle";
  $("#l-search").hidden = L.state === "loading";
  $("#l-search").textContent = searched ? "Search with the new tags" : "Search again";
  if (L.state === "loading") status.textContent = `Looking up "${t.title}"${t.artist ? " by " + t.artist : ""} on LRCLIB and Genius…`;
  else if (L.state === "fail") status.textContent = (L.error || "No lyrics found.") + " Paste your own below, or edit the tags and search again.";
  else if (sum) status.textContent = sum.query && !sum.query.artist ? "No artist known - check these are the right song." : "";
  else status.textContent = "";
  // the aligner reads Latin letters (English-trained): Japanese / Cyrillic / ... lyrics only get rough timing
  const shown = L.choice === "mine" ? L.mine : (L.texts[L.choice] || "");
  const letters = shown.replace(/\[[^\]]*\]|[\s\d\p{P}\p{S}]/gu, "");
  const latin = letters.replace(/[^\p{Script=Latin}]/gu, "");
  if (letters.length > 20 && latin.length / letters.length < 0.5)
    status.textContent = "These lyrics aren't in Latin letters, so the timing will be rough. A romanised version (paste it under My own) times better.";

  const opts = $("#l-options");
  opts.innerHTML = "";
  const add = (key, title, meta, ok, rec) => {
    const d = document.createElement("label");
    d.className = "opt" + (L.choice === key ? " sel" : "") + (ok ? "" : " off");
    d.innerHTML = `<input type="radio" name="lyr"><div class="name"></div><div class="meta"></div>`;
    d.querySelector(".name").textContent = title;
    if (rec) d.querySelector(".name").insertAdjacentHTML("beforeend", "<em>recommended</em>");
    d.querySelector(".meta").textContent = meta;
    const r = d.querySelector("input");
    r.checked = L.choice === key;
    r.disabled = !ok;
    r.onchange = () => { L.choice = key; render(); };
    opts.appendChild(d);
  };
  if (sum) {
    const l = sum.local, w = sum.web, b = sum.both;
    const mmss = (x) => x ? `${Math.floor(x / 60)}:${String(Math.round(x % 60)).padStart(2, "0")}` : "";
    add("local", "Local - LRCLIB", l.found ? `"${l.title}" by ${l.artist} · ${mmss(l.duration)} · ${l.lines} lines, timed` : l.error, l.found, sum.recommended === "local");
    add("web", "Web - Genius", w.found ? `"${w.title}" by ${w.artist} · ${w.lines} lines · carefully edited words, no timing` : w.error, w.found, sum.recommended === "web");
    add("both", "Both - Genius words on LRCLIB timing", b.found ? `${b.matched} lines matched, ${b.changed} with corrected words` : "needs both, for the same song", b.found, sum.recommended === "both");
  }
  if (L.state !== "loading") add("mine", "My own", "paste or type them", true, false);
  $("#l-mine").hidden = L.choice !== "mine";
  if (L.choice === "mine" && document.activeElement !== $("#l-mine")) $("#l-mine").value = L.mine;

  const diff = $("#l-diff");
  diff.hidden = !(L.choice === "both" && sum && sum.both.examples && sum.both.examples.length);
  if (!diff.hidden) {
    diff.innerHTML = "";
    for (const [g, l] of sum.both.examples.slice(0, 4)) {
      const d = document.createElement("div");
      d.innerHTML = "<b>Genius</b><span></span><br><b>LRCLIB</b><span></span>";
      d.querySelectorAll("span")[0].textContent = g;
      d.querySelectorAll("span")[1].textContent = l;
      diff.appendChild(d);
    }
  }
  const pre = $("#l-preview");
  const text = L.choice && L.choice !== "mine" ? L.texts[L.choice] : "";
  pre.hidden = !text;
  if (text) {
    pre.innerHTML = "";
    for (const line of text.split(/\r?\n/)) {
      if (!line.trim() || /^\[[a-z]+:/i.test(line)) continue;  // [ti:] / [ar:] headers
      const m = line.match(/^\[(\d+:\d+(?:\.\d+)?)\](.*)$/);
      const row = document.createElement("div");
      if (m) {
        const ts = document.createElement("span");
        ts.className = "ts";
        ts.textContent = m[1].replace(/\.\d+$/, "");
        row.appendChild(ts);
      }
      row.appendChild(document.createTextNode(m ? m[2] : line));
      pre.appendChild(row);
    }
  }

  $("#accent").value = s.accent;

  // result
  $("#d-result").hidden = !(s.status === "done" || s.status === "failed" || s.files.length);
  $("#d-error").hidden = !s.error;
  $("#d-error").textContent = s.error;
  const ul = $("#d-files");
  ul.innerHTML = "";
  for (const f of s.files) {
    const li = document.createElement("li");
    li.innerHTML = `<span></span><span><button class="link">Open</button><button class="link">Show in folder</button></span>`;
    li.firstChild.textContent = f.split(/[\\/]/).pop();
    const [open, reveal] = li.querySelectorAll("button");
    open.onclick = () => send({ cmd: "open", path: f });
    reveal.onclick = () => send({ cmd: "reveal", path: f });
    ul.appendChild(li);
  }
}

function renderBar() {
  $("#outdir").textContent = outDir || "Choose a folder…";
  $("#outdir").title = outDir;
  for (const c of document.querySelectorAll("[data-out]")) c.checked = outputs.includes(c.dataset.out);
}

function render() { renderList(); renderDetail(); renderBar(); }

// ---------------------------------------------------------------- events from the page

$("#browse").onclick = $("#add").onclick = () => send({ cmd: "browse" });
$("#d-remove").onclick = () => {
  const i = songs.findIndex((s) => s.id === selected);
  if (i < 0 || songs[i].status === "working" || songs[i].status === "queued") return;
  songs.splice(i, 1);
  selected = songs.length ? songs[Math.min(i, songs.length - 1)].id : null;
  render();
};
for (const r of document.querySelectorAll("input[name=tagmode]")) {
  r.onchange = () => {
    const s = song(selected);
    if (s.mode === "keep" && r.value === "own") s.own = { ...tagsOf(s) };  // start from what the file says
    s.mode = r.value;
    render();
  };
}
for (const k of ["title", "artist", "album"]) {
  $("#t-" + k).oninput = (e) => { const s = song(selected); s.own[k] = e.target.value; renderList(); renderDetail(); };
}
$("#t-pickcover").onclick = () => send({ cmd: "pickCover", id: selected });
$("#t-clearcover").onclick = () => { const s = song(selected); s.cover = null; s.coverUrl = null; render(); };
$("#l-search").onclick = () => { searchLyrics(song(selected)); render(); };
let mineTimer = 0;
$("#l-mine").oninput = (e) => {
  const s = song(selected), text = e.target.value;
  s.lyr.mine = text;
  s.lyr.mineFile = null;
  clearTimeout(mineTimer);
  mineTimer = setTimeout(() => {
    if (text.trim()) send({ cmd: "saveLyrics", id: s.id, text, synced: /^\s*\[\d+:\d+/m.test(text) });
    renderList();
  }, 400);
};
$("#accent").oninput = (e) => { song(selected).accent = e.target.value; };
for (const c of document.querySelectorAll("[data-out]")) {
  c.onchange = () => {
    outputs = [...document.querySelectorAll("[data-out]")].filter((x) => x.checked).map((x) => x.dataset.out);
    store.set("outputs", outputs);
    renderList();
  };
}
$("#outdir").onclick = () => send({ cmd: "pickFolder", current: outDir });
$("#start").onclick = () => {
  if (!outDirAsked || !outDir) { send({ cmd: "pickFolder", current: outDir, thenStart: true }); return; }
  start();
};
$("#cancel").onclick = () => send({ cmd: "cancel" });

function start() {
  const todo = songs.filter((s) => lyricsFile(s) && s.status !== "done" && s.status !== "working");
  if (!todo.length || busy) return;
  busy = true;
  for (const s of todo) { s.status = "queued"; s.pct = 0; s.files = []; s.error = ""; }
  send({
    cmd: "start",
    songs: todo.map((s) => {
      const t = tagsOf(s);
      return { id: s.id, path: s.path, title: t.title, artist: t.artist, album: t.album, accent: s.accent,
               cover: s.cover || "", lyricsFile: lyricsFile(s), outputs, outDir };
    }),
  });
  render();
}

// drag and drop: files dropped on the page go to the host with their paths
let dragDepth = 0;
window.addEventListener("dragenter", (e) => { e.preventDefault(); dragDepth++; $("#dragcover").hidden = false; });
window.addEventListener("dragleave", () => { if (--dragDepth <= 0) { dragDepth = 0; $("#dragcover").hidden = true; } });
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
  e.preventDefault();
  dragDepth = 0;
  $("#dragcover").hidden = true;
  if (e.dataTransfer.files.length && host) host.postMessageWithAdditionalObjects({ cmd: "drop" }, e.dataTransfer.files);
});

// ---------------------------------------------------------------- messages from the host

// overall progress of a song: analysis 40% (separation most of it), then the chosen files
const outputsShare = (s) => (s.outCount || 1);
function progressOf(s, m) {
  const f = m.total ? Math.min(1, m.done / m.total) : 0;
  if (m.phase === "analyze") {
    if (m.stage === "separate") { s.stage = "Separating the vocals"; return 0.33 * f; }
    if (m.stage === "beats") { s.stage = "Finding the beat"; return Math.max(s.pct, 0.05); }
    if (m.stage === "align") { s.stage = "Timing the lyrics"; return Math.max(s.pct, 0.33 + 0.07 * f); }
    s.stage = "Starting";
    return s.pct;
  }
  if (m.stage === "short") { s.stage = "Choosing the short"; return Math.max(s.pct, 0.4); }
  if (m.stage === "layout") { s.stage = `Laying out (${m.of})`; s.outIndex = m.done; s.outCount = m.total; return Math.max(s.pct, 0.4 + 0.6 * m.done / m.total); }
  if (m.stage === "render") {
    s.stage = `Rendering ${m.of}`;
    return Math.max(s.pct, 0.4 + 0.6 * ((s.outIndex || 0) + f) / outputsShare(s));
  }
  if (m.stage === "done") return 1;
  return s.pct;
}

const handlers = {
  init(m) {
    if (!outDir) { outDir = m.outDir; store.set("outDir", outDir); }
    if (m.testStart) {  // testing (MT_TEST_START=folder): make everything once the lyrics are in, no dialogs
      outDir = m.testStart;
      if (m.testOutputs) outputs = m.testOutputs.split(",");
      outDirAsked = true;
      const t = setInterval(() => {
        if (songs.length && songs.every((s) => s.lyr.state !== "loading" && s.status !== "reading")) { clearInterval(t); start(); }
      }, 1000);
    }
    render();
  },
  files(m) {
    for (const p of m.paths) addSong(p);
    render();
  },
  tags(m) {
    const s = song(m.id);
    if (!s) return;
    s.file = m.tags || {};
    if (m.tags && m.tags.coverPath) { s.cover = m.tags.coverPath; s.coverUrl = m.tags.coverUrl; }
    s.status = "idle";
    searchLyrics(s);
    render();
  },
  lyrics(m) {
    const s = song(m.id);
    if (!s) return;
    const L = s.lyr;
    L.state = m.ok ? "ok" : "fail";
    L.summary = m.ok ? m.summary : null;
    L.texts = m.texts || {};
    L.error = m.error ? m.error.replace(/^error:\s*/, "").replace(/ Pass --lyrics.*$/, "") : "";
    if (m.ok && (!L.choice || L.choice !== "mine")) L.choice = m.summary.recommended;
    if (!m.ok && !L.choice) L.choice = "mine";
    render();
  },
  devices(m) { $("#device").textContent = m.plan ? "Runs on: " + m.plan : ""; },
  savedLyrics(m) { const s = song(m.id); if (s) { s.lyr.mineFile = m.file; renderList(); } },
  cover(m) { const s = song(m.id); if (s) { s.cover = m.path; s.coverUrl = m.url; render(); } },
  folder(m) {
    if (m.path) { outDir = m.path; outDirAsked = true; store.set("outDir", outDir); }
    renderBar();
    if (m.path && m.thenStart) start();
  },
  progress(m) {
    const s = song(m.id);
    if (!s) return;
    s.status = "working";
    s.pct = progressOf(s, m);
    renderList();
    const done = songs.filter((x) => x.status === "done").length, all = songs.filter((x) => ["done", "working", "queued", "failed"].includes(x.status)).length;
    $("#overall").textContent = `${done} of ${all} done`;
  },
  file(m) { const s = song(m.id); if (s) { s.files.push(m.path); if (s.id === selected) renderDetail(); } },
  songDone(m) {
    const s = song(m.id);
    if (!s) return;
    s.status = m.ok ? "done" : "failed";
    s.pct = m.ok ? 1 : s.pct;
    s.error = m.ok ? "" : (m.error || "Something went wrong.") + (m.log ? `\n(details: ${m.log})` : "");
    render();
  },
  allDone(m) {
    busy = false;
    for (const s of songs) if (s.status === "queued" || s.status === "working") s.status = m.cancelled ? "idle" : s.status;
    $("#overall").textContent = m.cancelled ? "Stopped" : "All done";
    render();
  },
};

if (host) {
  host.addEventListener("message", (e) => { const m = e.data; if (handlers[m.type]) handlers[m.type](m); });
  send({ cmd: "init" });
}
render();
