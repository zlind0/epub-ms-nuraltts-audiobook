import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { listen } from "@tauri-apps/api/event";

const $ = (id) => document.getElementById(id);
const LS_KEY = "epub-mstts-settings-v1";
const state = {
  book: null, selected: new Set(), toggled: new Map(),
  expandDepth: 6, hasChildren: [], current: -1,
  preview: null, voices: [], running: false,
  prog: new Map(), // toc -> {doneChunks,totalChunks,phase}
  outDir: "", outDirManual: false, cores: 4,
};

// ---- 设置记忆(localStorage):音色/语速/音调/线程/码率/手动选过的输出目录 ----
function loadSettings() {
  try {
    const s = JSON.parse(localStorage.getItem(LS_KEY) || "{}");
    if (s.rate != null) { $("rate").value = s.rate; }
    if (s.pitch != null) { $("pitch").value = s.pitch; }
    if (s.threads != null) { $("threads").value = s.threads; }
    if (s.bitrate != null) { $("bitrate").value = s.bitrate; }
    if (s.outDir) { state.outDir = s.outDir; state.outDirManual = !!s.outDirManual; }
    $("rateV").textContent = Number($("rate").value).toFixed(2);
    $("pitchV").textContent = Number($("pitch").value).toFixed(2);
    $("threadsV").textContent = $("threads").value;
    return s;
  } catch { return {}; }
}
function saveSettings() {
  try {
    localStorage.setItem(LS_KEY, JSON.stringify({
      voice: $("voice").value || "",
      rate: Number($("rate").value),
      pitch: Number($("pitch").value),
      threads: Number($("threads").value),
      bitrate: Number($("bitrate").value),
      outDir: state.outDir || "",
      outDirManual: state.outDirManual
    }));
  } catch {}
}
const remembered = loadSettings();

// ---- 折叠逻辑(与 aurora EbookTocPage 同口径):自适应深度,默认可见<=200 ----
function countVisible(toc, expandFn) {
  const stack = [];
  let n = 0;
  for (let i = 0; i < toc.length; i++) {
    const lv = toc[i].level;
    while (stack.length && stack[stack.length - 1][1] >= lv) stack.pop();
    if (stack.every(([idx]) => expandFn(idx))) n++;
    stack.push([i, lv]);
  }
  return n;
}
function computeDepth(toc) {
  let d = 6;
  while (d > 0 && countVisible(toc, (i) => toc[i].level <= d) > 200) d--;
  return d;
}
function defaultExpanded(i) {
  return state.book.toc[i].level <= state.expandDepth;
}
function isExpanded(i) {
  const d = defaultExpanded(i);
  return state.toggled.has(i) ? !d : d;
}
function computeHasChildren(toc) {
  const h = new Array(toc.length).fill(false);
  for (let i = 0; i < toc.length; i++) {
    const lv = toc[i].level;
    for (let j = i + 1; j < toc.length; j++) {
      if (toc[j].level <= lv) break;
      if (toc[j].level === lv + 1) { h[i] = true; break; }
    }
  }
  return h;
}
function visibleRows() {
  const toc = state.book.toc;
  const stack = [];
  const rows = [];
  for (let i = 0; i < toc.length; i++) {
    const lv = toc[i].level;
    while (stack.length && stack[stack.length - 1][1] >= lv) stack.pop();
    const ok = stack.every(([idx]) => isExpanded(idx));
    stack.push([i, lv]);
    if (ok) rows.push(i);
  }
  return rows;
}
function descendants(i) {
  const toc = state.book.toc;
  const lv = toc[i].level;
  const out = [];
  for (let j = i + 1; j < toc.length; j++) {
    if (toc[j].level <= lv) break;
    out.push(j);
  }
  return out;
}

// ---- 树渲染:每行背景=该条目进度条 ----
function renderTree() {
  const el = $("tree");
  el.innerHTML = "";
  if (!state.book) { el.innerHTML = '<div class="muted">先打开一本 EPUB 电子书</div>'; return; }
  for (const i of visibleRows()) {
    const e = state.book.toc[i];
    const row = document.createElement("div");
    row.className = "trow" + (state.current === i ? " sel" : "");
    row.style.paddingLeft = (6 + (e.level - 1) * 18) + "px";
    const p = state.prog.get(i);
    if (p && p.totalChunks > 0) {
      const f = Math.min(1, p.doneChunks / p.totalChunks);
      const col = p.phase === "failed" ? "rgba(214,58,58,.30)"
        : p.phase === "done" ? "rgba(46,160,67,.30)"
        : p.phase === "skipped" ? "rgba(120,130,140,.30)"
        : "rgba(11,110,219,.28)";
      row.style.backgroundImage = `linear-gradient(to right, ${col} ${f * 100}%, transparent ${f * 100}%)`;
    }
    const tri = document.createElement("span");
    tri.className = "tri" + (state.hasChildren[i] ? "" : " leaf");
    tri.textContent = state.hasChildren[i] ? (isExpanded(i) ? "▾" : "▸") : "";
    tri.onclick = (ev) => {
      ev.stopPropagation();
      state.toggled.set(i, !(state.toggled.get(i) ?? false));
      if (state.toggled.get(i) === false) state.toggled.delete(i);
      // 清理无意义的 toggled(与默认一致则删除)
      renderTree();
    };
    const cb = document.createElement("input");
    cb.type = "checkbox";
    const desc = descendants(i);
    const all = [i, ...desc];
    const selN = all.filter((x) => state.selected.has(x)).length;
    cb.checked = selN === all.length && all.length > 0;
    cb.indeterminate = selN > 0 && selN < all.length;
    cb.onclick = (ev) => {
      ev.stopPropagation();
      if (state.selected.has(i) && selN === all.length) {
        for (const x of all) state.selected.delete(x);
      } else {
        for (const x of all) state.selected.add(x);
      }
      updateSelInfo(); renderTree();
    };
    const t = document.createElement("span");
    t.className = "t";
    t.textContent = e.title || "（无标题）";
    t.title = e.title;
    row.onclick = () => selectRow(i);
    row.append(tri, cb, t);
    if (p && p.totalChunks > 0) {
      const pct = document.createElement("span");
      pct.className = "pct";
      pct.textContent = p.phase === "done" ? "✓" : p.phase === "skipped" ? "跳过" : `${p.doneChunks}/${p.totalChunks}`;
      row.append(pct);
    }
    el.append(row);
  }
  updateSelInfo();
}
function updateSelInfo() {
  $("selInfo").textContent = state.book
    ? `已选 ${state.selected.size}/${state.book.toc.length} 节 · 默认展开适配(可见≤200,当前深度${state.expandDepth})`
    : "";
  $("btnStart").disabled = !state.book || state.selected.size === 0 || state.running;
}

async function selectRow(i) {
  state.current = i;
  renderTree();
  try {
    const seg = await invoke("get_segment", { bookId: state.book.id, tocIndex: i });
    state.preview = seg;
    $("prevTitle").textContent = seg.title;
    $("prevChars").textContent = `${seg.paragraphs.length} 段 · ${seg.chars} 字`;
    const pv = $("preview");
    pv.innerHTML = "";
    const h = document.createElement("h2");
    h.textContent = seg.title;
    pv.append(h);
    for (const p of seg.paragraphs) {
      const pe = document.createElement("p");
      pe.textContent = p;
      pv.append(pe);
    }
    $("btnPreview").disabled = seg.paragraphs.length === 0;
  } catch (e) { $("preview").textContent = String(e); }
}

// ---- 事件 ----
$("btnOpen").onclick = async () => {
  const f = await open({ filters: [{ name: "电子书", extensions: ["epub"] }], multiple: false });
  if (!f) return;
  const path = Array.isArray(f) ? f[0] : f;
  $("bookTitle").textContent = "解析中…";
  try {
    const info = await invoke("open_epub", { path });
    state.book = info;
    state.selected = new Set(info.toc.map((e) => e.index));
    state.toggled = new Map();
    state.prog = new Map();
    state.current = -1;
    state.expandDepth = computeDepth(info.toc);
    state.hasChildren = computeHasChildren(info.toc);
    // 默认输出目录:epub 书本所在目录(转换时在其下新建与书文件名同名的文件夹);
    // 用户手动选过目录则沿用记忆,不被新书覆盖
    if (!(state.outDirManual && state.outDir)) {
      state.outDir = info.dir || "";
    }
    $("outDir").value = state.outDir;
    $("bookTitle").textContent = `《${info.title}》`;
    $("bookSub").textContent = `${info.author || ""} · 共 ${info.total} 节`;
    renderTree();
    if (info.toc.length) selectRow(0);
  } catch (e) { $("bookTitle").textContent = "打开失败:" + e; }
};
$("btnOut").onclick = async () => {
  const d = await open({ directory: true, multiple: false });
  if (d) {
    state.outDir = Array.isArray(d) ? d[0] : d;
    state.outDirManual = true; // 手动选过:以后记住,不再跟随书本目录
    $("outDir").value = state.outDir;
    saveSettings();
  }
};
$("btnAll").onclick = () => {
  if (!state.book) return;
  state.selected = new Set(state.book.toc.map((e) => e.index));
  updateSelInfo(); renderTree();
};
$("btnNone").onclick = () => {
  state.selected = new Set();
  updateSelInfo(); renderTree();
};
$("btnExpand").onclick = () => {
  if (!state.book) return;
  // 全部展开:把默认深度提到6并清空 toggled(超200也展,用户主动)
  state.expandDepth = 6; state.toggled = new Map();
  renderTree();
};
$("btnCollapse").onclick = () => {
  if (!state.book) return;
  state.expandDepth = 1; state.toggled = new Map();
  renderTree();
};
$("rate").oninput = () => ($("rateV").textContent = Number($("rate").value).toFixed(2));
$("pitch").oninput = () => ($("pitchV").textContent = Number($("pitch").value).toFixed(2));
$("threads").oninput = () => ($("threadsV").textContent = $("threads").value);
for (const id of ["rate", "pitch", "threads", "bitrate", "voice"]) {
  $(id).addEventListener("change", saveSettings);
}

$("btnPreview").onclick = async () => {
  if (!state.preview) return;
  const text = [state.preview.title, ...state.preview.paragraphs].join("\n").slice(0, 600);
  $("btnPreview").disabled = true;
  $("btnPreview").textContent = "合成中…";
  try {
    const url = await invoke("preview_speech", {
      text, voice: $("voice").value,
      rate: Number($("rate").value), pitch: Number($("pitch").value),
    });
    $("audio").src = url;
    $("audio").play();
  } catch (e) { alert("试听失败:" + e); }
  $("btnPreview").disabled = false;
  $("btnPreview").textContent = "试听本节";
};

$("btnStart").onclick = async () => {
  if (!state.book || state.running) return;
  if (!state.outDir) { alert("先选择输出目录"); return; }
  state.running = true;
  state.prog = new Map();
  $("btnStart").disabled = true; $("btnCancel").disabled = false;
  $("status").textContent = "启动 TTS 池…";
  try {
    const msg = await invoke("start_export", {
      params: {
        bookId: state.book.id,
        indices: [...state.selected].sort((a, b) => a - b),
        outDir: state.outDir,
        voice: $("voice").value,
        rate: Number($("rate").value),
        pitch: Number($("pitch").value),
        threads: Number($("threads").value),
        bitrate: Number($("bitrate").value),
      },
    });
    $("status").textContent = msg;
  } catch (e) {
    $("status").textContent = "启动失败:" + e;
    state.running = false;
    $("btnStart").disabled = false; $("btnCancel").disabled = true;
  }
};
$("btnCancel").onclick = async () => {
  await invoke("cancel_export");
  $("status").textContent = "取消中…";
};

await listen("export-progress", (ev) => {
  const p = ev.payload;
  state.prog.set(p.toc, { doneChunks: p.doneChunks, totalChunks: p.totalChunks, phase: p.phase });
  const phaseName = { synth: "合成", encode: "编码", done: "完成", failed: "失败", skipped: "已存在跳过" }[p.phase] || p.phase;
  $("totalProg").textContent = `${p.done}/${p.total} 节 · ${phaseName} · ${p.title} (${p.doneChunks}/${p.totalChunks})`;
  renderTree();
});
await listen("export-done", (ev) => {
  const r = ev.payload;
  state.running = false;
  $("btnStart").disabled = false; $("btnCancel").disabled = true;
  $("status").textContent =
    (r.cancelled ? "已取消:" : "完成:") + `成功${r.done}节` +
    (r.skipped ? `，跳过${r.skipped}节（已存在）` : "") +
    (r.failed?.length ? `,失败${r.failed.length}节:${r.failed.slice(0, 3).join("; ")}` : "") +
    (r.folder ? ` → ${state.outDir}/${r.folder}` : "");
  renderTree(); updateSelInfo();
});

// ---- 初始化 ----
try {
  const voices = await invoke("list_voices");
  state.voices = voices;
  const sel = $("voice");
  sel.innerHTML = "";
  for (const v of voices) {
    const o = document.createElement("option");
    o.value = v.id;
    o.textContent = `${v.name} · ${v.locale}${v.installed ? "" : "(缺)"}`;
    // 记忆的音色优先(存在才用),否则默认晓晓
    if (remembered.voice ? v.id === remembered.voice : v.id === "zh-CN-XiaoxiaoNeural") o.selected = true;
    sel.append(o);
  }
  const cores = await invoke("cpu_cores");
  state.cores = cores;
  const tmax = Math.max(1, Math.min(32, cores));
  $("threads").max = tmax;
  // 记忆的线程数按本机核数钳制;无记忆则默认 min(4, max)
  const want = remembered.threads != null
    ? Math.min(Math.max(1, Math.round(remembered.threads)), tmax)
    : Math.min(4, tmax);
  $("threads").value = want;
  $("threadsV").textContent = $("threads").value;
  saveSettings();
} catch (e) { $("status").textContent = "初始化失败:" + e; }
// 记忆的手动输出目录:开屏即回填,没开书也能看到
if (state.outDir) { $("outDir").value = state.outDir; }
renderTree();
