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
let themes = [];         // ui/themes.json
let lastLook = store.get("look", { theme: "original", accent: "#ff6b4a" });  // new songs start from the last choice


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
    show: "original",                     // lyrics not in Latin letters: shown as written | "roman"ised
    theme: lastLook.theme, accent: lastLook.accent,
    bgMode: "record", bg: null,           // bg: the user's clip {path, name, url (still)}
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
  const v = s.lyr.summary && (s.lyr.summary.versions || []).find((x) => x.id === c);
  return v && v.file;
}

// the main script of some lyrics, as the engine names it (for pasted lyrics)
function scriptOf(text) {
  const n = (re) => (text.match(re) || []).length;
  const kana = n(/[\p{Script=Hiragana}\p{Script=Katakana}]/gu), han = n(/\p{Script=Han}/gu);
  const counts = {
    Latin: n(/\p{Script=Latin}/gu), Japanese: kana ? kana + han : 0, Chinese: kana ? 0 : han,
    Korean: n(/\p{Script=Hangul}/gu), Cyrillic: n(/\p{Script=Cyrillic}/gu), Greek: n(/\p{Script=Greek}/gu),
    Devanagari: n(/\p{Script=Devanagari}/gu), Tamil: n(/\p{Script=Tamil}/gu), Thai: n(/\p{Script=Thai}/gu),
    Arabic: n(/\p{Script=Arabic}/gu),
  };
  const [top, count] = Object.entries(counts).sort((a, b) => b[1] - a[1])[0];
  return count ? top : "Latin";
}

function searchLyrics(s) {
  const t = tagsOf(s);
  s.lyr.state = "loading";
  s.lyr.searchedFor = t.title + "|" + t.artist;
  send({ cmd: "lyrics", id: s.id, path: s.path, title: t.title, artist: t.artist });
}

// ready to make: lyrics chosen, and a clip picked if the song is to play over one
const isReady = (s) => !!lyricsFile(s) && (s.bgMode !== "clip" || !!s.bg);

function statusText(s) {
  if (s.status === "reading") return ["Reading…", ""];
  if (s.status === "queued") return ["Waiting", ""];
  if (s.status === "working") return [Math.round(s.pct * 100) + "%", ""];
  if (s.status === "done") return ["Done", "ok"];
  if (s.status === "failed") return ["Failed", "bad"];
  if (s.lyr.state === "loading") return ["Lyrics…", ""];
  if (!lyricsFile(s)) return ["No lyrics", "bad"];
  if (!isReady(s)) return ["No video", "bad"];
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
  const ready = songs.filter((s) => isReady(s) && s.status !== "done").length;
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
  const versions = (sum && sum.versions) || [];
  const status = $("#l-status");
  const searched = (t.title + "|" + t.artist) !== L.searchedFor && L.state !== "idle";
  $("#l-search").hidden = L.state === "loading";
  $("#l-search").textContent = searched ? "Search with the new tags" : "Search again";
  if (L.state === "loading") status.textContent = `Looking up "${t.title}"${t.artist ? " by " + t.artist : ""} on LRCLIB, NetEase, Bugs, Genius and lyrics.ovh…`;
  else if (L.state === "fail") status.textContent = (L.error || "No lyrics found.") + " Paste your own below, or edit the tags and search again.";
  else if (sum) status.textContent = sum.query && !sum.query.artist ? "No artist known - check these are the right song." : "";
  else status.textContent = "";

  const opts = $("#l-options");
  opts.innerHTML = "";
  const add = (key, title, meta, rec) => {
    const d = document.createElement("label");
    d.className = "opt" + (L.choice === key ? " sel" : "");
    d.innerHTML = `<input type="radio" name="lyr"><div class="name"></div><div class="meta"></div>`;
    d.querySelector(".name").textContent = title;
    if (rec) d.querySelector(".name").insertAdjacentHTML("beforeend", "<em>recommended</em>");
    d.querySelector(".meta").textContent = meta;
    const r = d.querySelector("input");
    r.checked = L.choice === key;
    r.onchange = () => { L.choice = key; render(); };
    opts.appendChild(d);
  };
  for (const v of versions) add(v.id, v.name, v.detail, v.id === sum.recommended);
  if (L.state !== "loading") add("mine", "My own", "paste or type them", false);
  const missing = $("#l-missing");
  missing.hidden = !(sum && sum.missing && sum.missing.length);
  if (!missing.hidden) missing.textContent = "Also searched: " + sum.missing.map((m) => `${m.name} (${m.error})`).join(" · ");
  $("#l-mine").hidden = L.choice !== "mine";
  if (L.choice === "mine" && document.activeElement !== $("#l-mine")) $("#l-mine").value = L.mine;

  // lyrics not in Latin letters: timed through their romanisation; shown as written or romanised
  const shown = L.choice === "mine" ? L.mine : (L.texts[L.choice] || "");
  const script = L.choice === "mine" ? scriptOf(shown) : (sum && sum.script) || "Latin";
  $("#l-show").hidden = script === "Latin" || !shown.trim();
  $("#l-script").textContent = `${script} lyrics: timed through their romanisation.`;
  for (const r of document.querySelectorAll("input[name=lshow]")) r.checked = r.value === s.show;

  const diff = $("#l-diff");
  const best = versions.find((v) => v.id === L.choice && v.examples && v.examples.length);
  diff.hidden = !best;
  if (best) {
    const name = (id) => (versions.find((v) => v.id === id) || { name: id }).name;
    diff.innerHTML = "";
    for (const [g, l] of best.examples.slice(0, 4)) {
      const d = document.createElement("div");
      d.innerHTML = "<b></b><span></span><br><b></b><span></span>";
      const [b1, b2] = d.querySelectorAll("b"), [s1, s2] = d.querySelectorAll("span");
      b1.textContent = name(best.words);
      b2.textContent = name(best.timing);
      s1.textContent = g;
      s2.textContent = l;
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

  renderLook(s);

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

function renderLook(s) {
  const box = $("#themes");
  box.innerHTML = "";
  let group = "";
  for (const th of themes) {
    if (th.group !== group) {
      group = th.group;
      const g = document.createElement("div");
      g.className = "theme-group";
      g.textContent = group === "classic" ? "Classic" : "Moods";
      box.appendChild(g);
    }
    // 'original' has no colours of its own: Music Tool's dark sleeve in the song's colour
    const c = th.accent ? th : { bg: "#0c0b10", accent: s.accent, record: s.accent + "99" };
    const b = document.createElement("button");
    b.className = "swatch" + (s.theme === th.id ? " sel" : "");
    b.title = th.name;
    b.innerHTML = `<span class="tile"><i></i><i></i><i></i></span><span class="nm"></span>`;
    const [bg, hi, rec] = b.querySelectorAll(".tile i");  // background, highlight, record
    bg.style.background = c.bg;
    hi.style.background = c.accent;
    rec.style.background = c.record;
    b.querySelector(".nm").textContent = th.name;
    b.onclick = () => { s.theme = th.id; remember(s); render(); };
    box.appendChild(b);
  }
  $("#accent-row").hidden = s.theme !== "original";
  $("#accent").value = s.accent;
  for (const r of document.querySelectorAll("input[name=bgmode]")) r.checked = r.value === s.bgMode;
  $("#bg-row").hidden = s.bgMode !== "clip";
  $("#bg-thumb").hidden = !(s.bg && s.bg.url);
  if (s.bg && s.bg.url) $("#bg-thumb").src = s.bg.url;
  $("#bg-name").textContent = s.bgError || (s.bg ? s.bg.name : "No video chosen yet");
  $("#bg-pick").textContent = s.bg ? "Change…" : "Choose…";
  $("#v-all").hidden = songs.length < 2;
}

function remember(s) { lastLook = { theme: s.theme, accent: s.accent }; store.set("look", lastLook); }

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
for (const r of document.querySelectorAll("input[name=lshow]")) r.onchange = () => { song(selected).show = r.value; };
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
$("#accent").oninput = (e) => { const s = song(selected); s.accent = e.target.value; remember(s); renderLook(s); };
for (const r of document.querySelectorAll("input[name=bgmode]")) {
  r.onchange = () => {
    const s = song(selected);
    s.bgMode = r.value;
    if (s.bgMode === "clip" && !s.bg) send({ cmd: "pickBackground", id: s.id });
    render();
  };
}
$("#bg-pick").onclick = () => send({ cmd: "pickBackground", id: selected });
$("#v-all").onclick = () => {
  const s = song(selected);
  for (const o of songs) {
    if (o.status === "working" || o.status === "queued") continue;
    Object.assign(o, { theme: s.theme, accent: s.accent, bgMode: s.bgMode, bg: s.bg });
  }
  render();
};
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
  const todo = songs.filter((s) => isReady(s) && s.status !== "done" && s.status !== "working");
  if (!todo.length || busy) return;
  busy = true;
  for (const s of todo) { s.status = "queued"; s.pct = 0; s.files = []; s.error = ""; }
  send({
    cmd: "start",
    songs: todo.map((s) => {
      const t = tagsOf(s);
      return { id: s.id, path: s.path, title: t.title, artist: t.artist, album: t.album, accent: s.accent,
               theme: s.theme, background: s.bgMode === "clip" && s.bg ? s.bg.path : "", show: s.show,
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
  if (m.stage === "background") { s.stage = `Preparing your video (${m.of})`; return s.pct; }
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
    testBackground = m.testBackground || "";
    if (m.testStart) {  // testing (MT_TEST_START=folder): make everything once the lyrics are in, no dialogs
      outDir = m.testStart;
      if (m.testOutputs) outputs = m.testOutputs.split(",");
      if (m.testTheme) lastLook.theme = m.testTheme;
      outDirAsked = true;
      const t = setInterval(() => {
        if (songs.length && songs.every((s) => s.lyr.state !== "loading" && s.status !== "reading")) { clearInterval(t); start(); }
      }, 1000);
    }
    render();
  },
  files(m) {
    for (const p of m.paths) addSong(p);
    if (testBackground) for (const s of songs) if (!s.bg) { s.bgMode = "clip"; send({ cmd: "pickBackground", id: s.id, path: testBackground }); }
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
  background(m) {
    const s = song(m.id);
    if (!s) return;
    s.bgError = m.ok ? "" : `Couldn't read ${m.name} as a video or picture. Try another file.`;
    if (m.ok) { s.bg = { path: m.path, name: m.name, url: m.url }; s.bgMode = "clip"; }
    render();
  },
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

let testBackground = "";  // testing (MT_TEST_BACKGROUND=clip): every song over this clip
fetch("themes.json").then((r) => r.json()).then((j) => { themes = j.themes; render(); }).catch(() => {});

if (host) {
  host.addEventListener("message", (e) => { const m = e.data; if (handlers[m.type]) handlers[m.type](m); });
  send({ cmd: "init" });
}
render();
