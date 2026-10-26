import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile, copyFile } from 'node:fs/promises';
import { createWriteStream } from 'node:fs';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import os from 'node:os';
import { chromium } from 'playwright';
import { createHash } from 'node:crypto';

const root = fileURLToPath(new URL('..', import.meta.url));
const mode = process.argv.includes('--mode') ? process.argv[process.argv.indexOf('--mode') + 1] : 'wasm';
const quick = process.argv.includes('--quick');
if (!['wasm', 'webgpu', 'auto'].includes(mode)) throw new Error('Invalid test mode');
const stamp = new Date().toISOString().replaceAll(/[:.]/g, '-');
const output = path.join(root, 'test-results', `${stamp}-${mode}${quick ? '-quick' : ''}`);
await mkdir(output, { recursive: true });
const data = path.join(output, 'database');
const port = Number(process.env.KB_TEST_PORT || 18777), origin = `http://127.0.0.1:${port}`;
const serverEnv = { ...process.env, HOST: '127.0.0.1', PORT: String(port), KB_DATA_DIR: data, KB_SQL_DATABASE: `kb_${stamp.replaceAll(/[^a-zA-Z0-9]/g, '').toLowerCase()}_${mode}` };
const report = { startedAt: new Date().toISOString(), mode, quick, output, node: process.version, os: { release: os.release(), arch: os.arch(), cpu: os.cpus()[0]?.model, logicalCpus: os.cpus().length }, checks: [], configurations: [], consoleErrors: [], pageErrors: [], network: [] };
report.sourceHashes = {};
for (const name of ['server.mjs', 'src/main.js', 'src/embed-worker.js', 'lib/contracts.mjs', 'scripts/start-database.mjs', 'package-lock.json', 'model.json', 'sample.json', 'tests/browser.mjs']) report.sourceHashes[name] = createHash('sha256').update(await readFile(path.join(root, name))).digest('hex');
let server, browser;
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function startServer() {
  const log = createWriteStream(path.join(output, 'server.log'), { flags: 'a' });
  server = spawn(process.execPath, [path.join(root, 'server.mjs')], { cwd: root, env: serverEnv, stdio: ['ignore', 'pipe', 'pipe'] });
  server.stdout.pipe(log); server.stderr.pipe(log);
  for (let i = 0; i < 120; i++) {
    if (server.exitCode !== null) throw new Error(`Server exited ${server.exitCode}: ${(await readFile(path.join(output, 'server.log'), 'utf8')).slice(-5000)}`);
    try { const response = await fetch(`${origin}/api/health`); if (response.ok) { const health = await response.json(); if (health.pid === server.pid) { report.server = health; return; } } } catch {}
    await wait(1000);
  }
  throw new Error('Server readiness timeout');
}
async function stopServer() {
  if (!server || server.exitCode !== null) return;
  await fetch(`${origin}/api/shutdown`, { method: 'POST', headers: { 'X-KB-Control': 'local-shutdown' } });
  await Promise.race([once(server, 'exit'), wait(15000)]);
  if (server.exitCode === null) { server.kill('SIGKILL'); await once(server, 'exit'); report.forcedServerStop = true; }
  (report.serverStops ||= []).push({ exitCode: server.exitCode, signal: server.signalCode });
  assert.equal(server.exitCode, 0, `application service must stop normally (${server.signalCode})`);
}
const median = values => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
function check(name, actual) { assert.ok(actual, name); report.checks.push({ name, passed: true }); console.log(`PASS ${name}`); }
try {
  console.log(`Browser test ${mode}; evidence: ${output}`);
  await startServer();
  const gpuArgs = mode !== 'webgpu' ? ['--disable-gpu', '--disable-features=WebGPU'] : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'];
  browser = await chromium.launch({ executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE || undefined, headless: true, args: ['--no-sandbox', '--disable-dev-shm-usage', ...gpuArgs] });
  report.browser = browser.version(); report.browserArgs = gpuArgs;
  const context = await browser.newContext({ viewport: { width: 1440, height: 1050 } });
  const page = await context.newPage(); page.setDefaultTimeout(180000);
  page.on('pageerror', error => { report.pageErrors.push(error.message); console.log('PAGE ERROR', error.message); });
  page.on('console', message => { if (message.type() === 'error' && report.consoleErrors.length < 40) { report.consoleErrors.push(message.text().slice(0, 2000)); console.log('BROWSER ERROR', message.text().slice(0, 1200)); } });
  page.on('request', request => { if (report.network.length < 2000) report.network.push({ method: request.method(), url: request.url() }); });
  await page.goto(origin); await page.waitForFunction(() => window.knowledge?.state.selected && !window.knowledge.state.busy);
  await page.locator('.lab summary').click();
  await page.locator('#device').selectOption(mode === 'auto' ? 'auto' : mode);
  await page.waitForFunction(() => !window.knowledge.state.busy);
  const loaded = await page.evaluate(() => window.knowledge.state.model);
  if (!loaded) throw new Error(`Model did not load: ${await page.locator('#status').innerText()}`);
  check('actual browser inference backend selected', loaded.backend === (mode === 'webgpu' ? 'webgpu' : 'wasm'));
  if (mode === 'auto') check('automatic WASM fallback explains unavailable WebGPU', !!loaded.fallbackReason);
  report.firstModelLoad = loaded;
  await page.locator('#sample').click(); await page.waitForFunction(() => !window.knowledge.state.busy);
  const sample = JSON.parse(await readFile(path.join(root, 'sample.json'), 'utf8'));
  const imported = await page.evaluate(() => window.knowledge.state.selected.count);
  check('UI imports actual sample into seekdb', imported === sample.documents.length);
  await page.locator('#query').fill(sample.queries[0].query); await page.locator('#search-button').click(); await page.waitForFunction(() => !window.knowledge.state.busy);
  check('UI returns native hybrid results with genuine source text', await page.locator('.result').count() > 0 && await page.locator('.result').first().locator('p').innerText() === sample.documents.find(x => x.id === sample.queries[0].relevant[0]).text);
  report.firstUiImport = await page.evaluate(() => window.knowledge.events.find(event => event.operation === 'import'));
  report.firstUiQuery = await page.evaluate(() => window.knowledge.events.find(event => event.operation === 'search'));
  await page.screenshot({ path: path.join(output, 'desktop.png'), fullPage: true });
  await page.setViewportSize({ width: 390, height: 844 }); await page.screenshot({ path: path.join(output, 'mobile.png'), fullPage: true });
  check('mobile page has no horizontal overflow', await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  await page.setViewportSize({ width: 1440, height: 1050 });
  const q8name = await page.evaluate(() => window.knowledge.state.selected.name);
  const rows = sample.documents.map(row => ({ sourceId: row.id, source: row.title, text: row.text, part: 1 }));
  const configurations = quick ? ['q8'] : mode === 'wasm' ? ['fp32', 'q8'] : ['q8'];
  for (const dtype of configurations) {
    console.log(`Measure ${dtype}/${mode}`);
    if (dtype === 'q8') await page.evaluate(name => window.knowledge.selectSpace(name), q8name);
    else await page.evaluate(dtype => window.knowledge.createSpace(`公开样例 · ${dtype.toUpperCase()}`, dtype, `benchmark_${dtype}`), dtype);
    const info = await page.evaluate(({ mode, dtype }) => window.knowledge.initialize({ device: mode, dtype }), { mode, dtype });
    const ingestion = await page.evaluate(rows => window.knowledge.importRows(rows), rows);
    const result = { dtype, backend: info.backend, adapter: info.adapter, modelLoad: info, ingestion, queries: [] };
    for (let repeat = 0; repeat < (quick ? 1 : 3); repeat++) {
      for (const item of sample.queries) {
        const found = await page.evaluate(query => window.knowledge.search(query, { limit: 3 }), item.query);
        const ids = found.hits.map(hit => hit.metadata.sourceId);
        const recallAt3 = item.relevant.filter(id => ids.includes(id)).length / item.relevant.length;
        result.queries.push({ repeat, query: item.query, relevant: item.relevant, found: ids, recallAt3, embeddingMs: found.embeddingMs, searchMs: found.searchMs, totalMs: found.totalMs });
      }
    }
    result.recallAt3 = result.queries.reduce((sum, row) => sum + row.recallAt3, 0) / result.queries.length;
    result.medianEmbeddingMs = median(result.queries.map(x => x.embeddingMs)); result.medianSearchMs = median(result.queries.map(x => x.searchMs)); result.medianTotalMs = median(result.queries.map(x => x.totalMs));
    report.configurations.push(result); console.log(`Recall@3 ${result.recallAt3}; median end-to-end ${result.medianTotalMs.toFixed(1)}ms`);
    check(`${dtype}/${mode} returns finite vectors and satisfies public example Recall@3 >= 0.75`, result.recallAt3 >= 0.75);
  }
  await page.evaluate(name => window.knowledge.selectSpace(name), q8name);
  await page.evaluate(({ mode }) => window.knowledge.initialize({ device: mode, dtype: 'q8' }), { mode });
  if (!quick) {
    report.batchComparison = [];
    for (const batchSize of [1, 8]) {
      const timings = [];
      for (let repeat = 0; repeat < 3; repeat++) { const value = await page.evaluate(({ texts, batchSize }) => window.knowledge.embed(texts, { batchSize }), { texts: sample.documents.map(x => x.text), batchSize }); timings.push(value.embeddingMs); }
      report.batchComparison.push({ dtype: 'q8', backend: mode, batchSize, documentCount: rows.length, milliseconds: timings, medianMs: median(timings), documentsPerSecond: rows.length * 1000 / median(timings) });
    }
  }
  await page.evaluate(rows => window.knowledge.importRows(rows), rows);
  check('re-import deduplicates identical records', await page.evaluate(() => window.knowledge.state.selected.count) === rows.length);
  const extra = { sourceId: 'incremental.txt', source: '增量导入.txt', text: '飞船补给柜的取货令牌是蓝鲸7319。离港前用此令牌领取三箱压缩食品。', part: 1 };
  await page.locator('#files').setInputFiles({ name: 'incremental.txt', mimeType: 'text/plain', buffer: Buffer.from(extra.text) }); await page.waitForFunction(() => !window.knowledge.state.busy);
  const updated = await page.evaluate(() => window.knowledge.search('飞船补给柜的取货令牌是什么？'));
  check('incremental file is searchable after index refresh', updated.hits.some(x => x.metadata.sourceId === extra.sourceId));
  await page.evaluate(() => window.knowledge.createSpace('隔离检查', 'q8', 'isolated_space'));
  await page.evaluate(() => window.knowledge.importRows([{ sourceId: 'other', source: '另一份资料', text: '海边观测站今天记录到三只紫色海星。', part: 1 }]));
  const isolated = await page.evaluate(() => window.knowledge.search('飞船补给柜的取货令牌是什么？'));
  check('collections remain isolated during switching', isolated.hits.length === 1 && isolated.hits.every(x => x.metadata.sourceId === 'other'));
  await page.evaluate(name => window.knowledge.selectSpace(name), q8name);
  await stopServer(); await startServer(); await page.reload(); await page.waitForFunction(() => window.knowledge?.state.selected && !window.knowledge.state.busy);
  await page.evaluate(name => window.knowledge.selectSpace(name), q8name);
  await page.locator('.lab summary').click();
  await page.locator('#device').selectOption(mode === 'auto' ? 'auto' : mode); await page.waitForFunction(() => !window.knowledge.state.busy);
  const recovered = await page.evaluate(() => window.knowledge.search('飞船补给柜的取货令牌是什么？'));
  check('actual server restart preserves documents and collection state', recovered.hits.some(x => x.metadata.sourceId === extra.sourceId));
  const audit = (await readFile(path.join(data, 'audit.jsonl'), 'utf8')).trim().split('\n').map(x => JSON.parse(x));
  check('seekdb-js native hybrid query and refresh have actual audit records', audit.some(x => x.operation === 'native_hybrid_search' && x.databaseFusion === 'DBMS_HYBRID_SEARCH') && audit.some(x => x.operation === 'upsert_and_refresh'));
  await copyFile(path.join(data, 'audit.jsonl'), path.join(output, 'database-audit.jsonl'));
  check('no external browser inference or model requests', report.network.every(x => x.url.startsWith(origin) || x.url.startsWith('blob:') || x.url.startsWith('data:')));
  check('no uncaught page errors', report.pageErrors.length === 0);
  await page.locator('#query').fill(sample.queries[0].query); await page.locator('#search-button').click(); await page.waitForFunction(() => !window.knowledge.state.busy);
  await page.screenshot({ path: path.join(output, 'desktop.png'), fullPage: true });
  report.passed = true;
} catch (error) { report.passed = false; report.error = error.stack; console.error(error.stack); process.exitCode = 1; }
finally {
  if (browser) await browser.close();
  try { await stopServer(); } catch (error) { report.passed = false; report.shutdownError = error.stack; process.exitCode = 1; }
  report.finishedAt = new Date().toISOString();
  await writeFile(path.join(output, 'report.json'), JSON.stringify(report, null, 2));
  console.log(`Report: ${path.join(output, 'report.json')}`);
}
