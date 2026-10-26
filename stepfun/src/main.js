import './style.css';

const state = { spaces: [], selected: null, model: null, busy: false, status: '准备就绪', hits: [], metrics: null };
const worker = new Worker(new URL('./embed-worker.js', import.meta.url), { type: 'module' });
const pending = new Map(); let nextId = 1;
worker.onmessage = ({ data }) => {
  if (data.progress) {
    const p = data.progress;
    state.status = p.status === 'embedding' ? `本地向量化 ${p.done} / ${p.total}` : p.status === 'progress' ? `加载模型 ${Math.round(p.progress || 0)}%` : '正在初始化本地模型…';
    paintStatus(); return;
  }
  const request = pending.get(data.id); if (!request) return;
  pending.delete(data.id); data.error ? request.reject(new Error(data.error)) : request.resolve(data.result);
};
worker.onerror = event => { for (const entry of pending.values()) entry.reject(new Error(event.message || '推理 Worker 启动失败')); pending.clear(); };
function rpc(operation, input) { return new Promise((resolve, reject) => { const id = nextId++; pending.set(id, { resolve, reject }); worker.postMessage({ id, operation, input }); }); }
export async function api(url, input, method) {
  const response = await fetch(`/api${url}`, { method: method || (input ? 'POST' : 'GET'), headers: input ? { 'Content-Type': 'application/json' } : {}, body: input ? JSON.stringify(input) : undefined });
  const result = await response.json(); if (!response.ok) throw new Error(result.error || '服务请求失败'); return result;
}
const escape = value => String(value ?? '').replace(/[&<>"']/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch]);
document.querySelector('#app').innerHTML = `
  <aside class="sidebar"><a class="brand" href="/"><span class="brand-icon">近</span><span>近知<small>LOCAL KNOWLEDGE</small></span></a>
    <div class="section-label">我的知识库 <button id="new-space" aria-label="新建知识库">＋</button></div><nav id="spaces" aria-label="知识库"></nav>
    <div class="sidebar-foot"><span class="green-dot"></span> 本地计算，资料由你掌控<small>向量化在浏览器完成<br>全文与语义检索由 seekdb 完成</small></div>
  </aside>
  <main><header><div class="breadcrumb">工作台 <span>/</span> <strong id="space-label">我的资料</strong></div><span id="backend" class="pill">模型尚未加载</span></header>
    <section class="intro"><div class="eyebrow">LESS SEARCHING, MORE KNOWING</div><h1>把资料留在手边。</h1><p>导入一份笔记，问一个问题。找到答案，也找到它的出处。</p></section>
    <div class="workspace"><section class="panel import-panel"><div class="panel-heading"><h2>01 <span>收好资料</span></h2><span id="doc-count" class="counter">0 段</span></div>
      <label class="dropzone" for="files"><span class="upload-icon">↥</span><strong>选择你的资料</strong><span>支持 TXT、Markdown，可多选</span><small>文本会分段，在浏览器本地生成向量</small></label><input type="file" id="files" accept=".txt,.md,text/plain,text/markdown" multiple hidden>
      <label class="field-label" for="note">或者，粘贴一段内容</label><textarea id="note" placeholder="课程笔记、读书摘录、项目说明……" maxlength="30000"></textarea><button class="primary" id="import-note">导入这段内容 <span>↗</span></button>
      <div class="divider"><span>先体验一下</span></div><button id="sample" class="secondary">导入示例资料 <span>＋</span></button><div class="status" role="status" aria-live="polite"><span id="status-dot" class="green-dot"></span><span id="status">准备就绪</span></div>
      <details class="lab"><summary>推理设置与实验</summary><label>执行后端<select id="device"><option value="auto">自动：优先 WebGPU</option><option value="wasm">WASM</option><option value="webgpu">WebGPU（验证模式）</option></select></label><p>集合精度固定，切换知识库会加载对应模型，避免混用向量。</p><pre id="model-info">等待加载模型</pre><button id="export-session" class="text-button">导出本次会话指标 ↓</button></details>
    </section>
    <section class="search-panel"><div class="panel-heading"><h2>02 <span>找到关联</span></h2><span class="subtle">全文 + 语义</span></div><form id="search-form" class="search-box"><span class="search-icon">⌕</span><input id="query" aria-label="检索问题" placeholder="想从资料中找到什么？" maxlength="600" required><button id="search-button" type="submit" aria-label="搜索">↗</button></form>
      <div class="suggestions"><button data-query="怎样让手冲咖啡不那么酸？">手冲咖啡不那么酸</button><button data-query="怎样防止重复请求造成重复扣款？">重复请求与幂等性</button></div><div id="search-summary" class="search-summary">每条结果都保留原文和来源</div><div id="results"><div class="empty"><div class="empty-mark">⌘</div><h3>让问题找到它的资料</h3><p>先导入内容，再输入一个问题。<br>你的资料不会发送到云端推理服务。</p></div></div>
    </section></div><footer>BGE-small-zh · 浏览器本地推理 <span>文档与向量存储于本机 seekdb</span></footer>
  </main>
  <dialog id="create-dialog"><form id="create-form"><span class="eyebrow">A SPACE FOR YOUR IDEAS</span><h2>新建知识库</h2><label>名称<input id="new-label" maxlength="100" placeholder="例如：读书与项目" required></label><label>模型精度<select id="new-dtype"><option value="q8">INT8 · 推荐，体积更小</option><option value="fp32">FP32 · 未量化对照</option></select></label><p>每个知识库独立保存资料。不同精度的向量不会混在一起。</p><div class="dialog-actions"><button type="button" id="cancel-create" class="secondary">取消</button><button type="submit" class="primary">创建</button></div></form></dialog>`;
const $ = selector => document.querySelector(selector);
const events = [];
function paintStatus() {
  $('#status').textContent = state.status;
  $('#status-dot').classList.toggle('busy-dot', state.busy);
  document.querySelectorAll('button, select, input, textarea').forEach(element => { element.disabled = state.busy; });
  $('#backend').textContent = state.model ? `${state.model.backend.toUpperCase()} · ${state.model.dtype.toUpperCase()}` : '模型尚未加载';
  $('#backend').classList.toggle('ready', !!state.model);
  if (state.model) $('#model-info').textContent = JSON.stringify(state.model, null, 2);
}
function paintSpaces() {
  $('#spaces').innerHTML = state.spaces.map(space => `<button data-space="${space.name}" class="space ${space.name === state.selected?.name ? 'active' : ''}"><span class="space-glyph">▤</span><span>${escape(space.label)}<small>${space.count} 段 · ${space.dtype.toUpperCase()}</small></span></button>`).join('');
  $('#space-label').textContent = state.selected?.label || '我的资料';
  $('#doc-count').textContent = `${state.selected?.count || 0} 段`;
  paintStatus();
}
function paintResults() {
  if (!state.metrics) { $('#results').innerHTML = '<div class="empty"><div class="empty-mark">⌘</div><h3>让问题找到它的资料</h3><p>先导入内容，再输入一个问题。</p></div>'; $('#search-summary').textContent = '每条结果都保留原文和来源'; return; }
  $('#search-summary').textContent = `${state.hits.length} 条结果 · 向量化 ${Math.round(state.metrics.embeddingMs)} ms · 检索 ${Math.round(state.metrics.searchMs)} ms · 全程 ${Math.round(state.metrics.totalMs)} ms`;
  $('#results').innerHTML = state.hits.length ? state.hits.map((hit, index) => `<article class="result"><div class="result-top"><span class="rank">${String(index + 1).padStart(2, '0')}</span><span>${escape(hit.metadata?.source || '导入资料')} · 第 ${escape(hit.metadata?.part || 1)} 段</span><span class="tag">混合检索</span></div><p>${escape(hit.text)}</p></article>`).join('') : '<div class="empty"><h3>这个知识库还没有匹配资料</h3><p>请确认已导入内容，或换一种问法。</p></div>';
}
async function refreshSpaces() { state.spaces = (await api('/spaces')).spaces; if (state.selected) state.selected = state.spaces.find(x => x.name === state.selected.name) || null; paintSpaces(); return state.spaces; }
async function createSpace(label, dtype = 'q8', name = `s_${crypto.randomUUID().replaceAll('-', '').slice(0, 20)}`) {
  const space = await api('/spaces', { name, label, dtype }); await refreshSpaces(); await selectSpace(space.name); return space;
}
async function selectSpace(name) {
  const space = state.spaces.find(x => x.name === name); if (!space) throw new Error('知识库不存在');
  state.selected = space; state.hits = []; state.metrics = null; $('#query').value = ''; paintSpaces(); paintResults(); return space;
}
async function initialize({ device = $('#device').value, dtype = state.selected?.dtype || 'q8' } = {}) {
  state.status = '正在加载本地模型…'; paintStatus();
  state.model = await rpc('initialize', { device, dtype });
  events.push({ operation: 'initialize', ...state.model });
  state.status = state.model.fallbackReason ? `已使用 WASM：${state.model.fallbackReason}` : `${state.model.backend.toUpperCase()} 模型已就绪`;
  paintStatus(); return state.model;
}
async function embed(texts, { batchSize = 8, query = false } = {}) { return rpc('embed', { texts, batchSize, query }); }
function splitText(text, sourceId, source = sourceId) {
  const paragraphs = text.replaceAll('\r\n', '\n').split(/\n\s*\n/).map(x => x.trim()).filter(Boolean), rows = [];
  for (const paragraph of paragraphs) for (let offset = 0; offset < paragraph.length; offset += 280) rows.push({ sourceId, source, text: paragraph.slice(offset, offset + 320), part: rows.length + 1 });
  return rows;
}
async function importRows(rows, { batchSize = 8 } = {}) {
  if (!state.selected) throw new Error('请先选择知识库');
  if (!rows.length) throw new Error('没有可导入的非空文本');
  if (rows.length > 400) throw new Error('一次最多导入 400 个段落，请分批选择文件');
  const space = state.selected, started = performance.now(); await initialize({ dtype: space.dtype });
  let writeMs = 0, refreshMs = 0, embeddingMs = 0;
  for (let offset = 0; offset < rows.length; offset += 32) {
    const batch = rows.slice(offset, offset + 32), encoded = await embed(batch.map(x => x.text), { batchSize }); embeddingMs += encoded.embeddingMs;
    state.status = `正在写入并刷新索引 ${Math.min(offset + 32, rows.length)} / ${rows.length}`; paintStatus();
    const written = await api(`/spaces/${space.name}/documents`, { dtype: space.dtype, documents: batch.map((row, i) => ({ ...row, embedding: encoded.embeddings[i] })) }); writeMs += written.writeMs; refreshMs += written.refreshMs;
  }
  await refreshSpaces(); const result = { operation: 'import', name: space.name, rows: rows.length, embeddingMs, writeMs, refreshMs, totalMs: performance.now() - started, backend: state.model.backend, dtype: space.dtype };
  events.push(result); state.status = `已导入 ${rows.length} 段，索引刷新完成`; paintStatus(); return result;
}
async function search(query, { limit = 5 } = {}) {
  if (!state.selected) throw new Error('请先选择知识库');
  if (!query.trim()) throw new Error('请输入检索问题');
  const space = state.selected, started = performance.now(); await initialize({ dtype: space.dtype });
  const encoded = await embed([query], { query: true });
  const found = await api(`/spaces/${space.name}/search`, { query, embedding: encoded.embeddings[0], dtype: space.dtype, limit });
  const result = { ...found, operation: 'search', query, name: space.name, backend: encoded.backend, dtype: space.dtype, embeddingMs: encoded.embeddingMs, totalMs: performance.now() - started };
  events.push(result); if (state.selected?.name === space.name) { state.hits = found.hits; state.metrics = result; paintResults(); }
  state.status = '检索完成，原文已就位'; paintStatus(); return result;
}
async function run(operation) {
  if (state.busy) return; state.busy = true; $('#status').classList.remove('error'); paintStatus();
  try { return await operation(); } catch (error) { state.status = error.message; $('#status').classList.add('error'); throw error; }
  finally { state.busy = false; paintStatus(); }
}
function click(operation) { return () => run(operation).catch(console.error); }
$('#new-space').onclick = () => $('#create-dialog').showModal();
$('#cancel-create').onclick = () => $('#create-dialog').close();
$('#create-form').onsubmit = event => { event.preventDefault(); run(async () => { await createSpace($('#new-label').value, $('#new-dtype').value); $('#create-dialog').close(); $('#new-label').value = ''; state.status = '新知识库已就绪，开始导入资料吧'; }).catch(console.error); };
$('#spaces').onclick = event => { const button = event.target.closest('[data-space]'); if (button) run(() => selectSpace(button.dataset.space)).catch(console.error); };
$('#import-note').onclick = click(() => importRows(splitText($('#note').value, '粘贴笔记', '粘贴笔记')));
$('#files').onchange = () => run(async () => {
  const rows = []; for (const file of $('#files').files) { if (file.size > 512000) throw new Error('单个文件最多 500 KiB'); rows.push(...splitText(await file.text(), file.name, file.name)); } await importRows(rows); $('#files').value = '';
}).catch(console.error);
$('#sample').onclick = click(async () => { const sample = await (await fetch('/sample.json')).json(); await importRows(sample.documents.map(row => ({ sourceId: row.id, source: row.title, text: row.text, part: 1 }))); });
$('#search-form').onsubmit = event => { event.preventDefault(); run(() => search($('#query').value)).catch(console.error); };
document.querySelector('.suggestions').onclick = event => { const button = event.target.closest('[data-query]'); if (button) { $('#query').value = button.dataset.query; run(() => search(button.dataset.query)).catch(console.error); } };
$('#device').onchange = () => run(() => initialize()).catch(console.error);
$('#export-session').onclick = () => { const blob = new Blob([JSON.stringify({ at: new Date().toISOString(), model: state.model, events }, null, 2)], { type: 'application/json' }); const link = document.createElement('a'); link.href = URL.createObjectURL(blob); link.download = 'near-knowledge-session.json'; link.click(); setTimeout(() => URL.revokeObjectURL(link.href), 1000); };
window.knowledge = { api, state, events, initialize, embed, createSpace, selectSpace, importRows, search, splitText, refreshSpaces };
await run(async () => { await refreshSpaces(); const preferred = localStorage.getItem('near-knowledge-space'); if (!state.spaces.length) await createSpace('我的资料', 'q8', 'my_notes'); else await selectSpace(state.spaces.find(x => x.name === preferred)?.name || state.spaces[0].name); state.status = '准备就绪，导入资料后即可检索'; }).catch(console.error);
window.addEventListener('beforeunload', () => { if (state.selected) localStorage.setItem('near-knowledge-space', state.selected.name); });
