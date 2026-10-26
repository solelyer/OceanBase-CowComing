import http from 'node:http';
import { readFile, appendFile, mkdir, stat } from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { createRequire } from 'node:module';
import { SeekdbClient, Schema, FulltextIndexConfig, VectorIndexConfig } from 'seekdb';
import { documents, dtypeName, spaceName, vector } from './lib/contracts.mjs';

const root = path.dirname(fileURLToPath(import.meta.url));
const data = path.resolve(process.env.KB_DATA_DIR || path.join(root, '.data'));
await mkdir(data, { recursive: true });
const model = JSON.parse(await readFile(path.join(root, 'model.json'), 'utf8'));
const require = createRequire(import.meta.url);
const ortDirectory = path.dirname(require.resolve('onnxruntime-web'));
const connection = { host: process.env.SEEKDB_HOST || '127.0.0.1', port: Number(process.env.SEEKDB_PORT || 12891), user: process.env.SEEKDB_USER || 'root', password: process.env.SEEKDB_PASSWORD || '', queryTimeout: 30000 };
const databaseName = process.env.KB_SQL_DATABASE || 'near_knowledge';
if (!/^[a-z][a-z0-9_]{0,63}$/.test(databaseName)) throw new Error('Invalid SQL database name');
const bootstrap = new SeekdbClient({ ...connection, database: 'test' });
const dbVersion = await bootstrap.execute('SELECT VERSION() AS version');
await bootstrap.execute(`CREATE DATABASE IF NOT EXISTS \`${databaseName}\``);
await bootstrap.close();
const client = new SeekdbClient({ ...connection, database: databaseName });
await client.execute('CREATE TABLE IF NOT EXISTS browser_kb_spaces (name VARCHAR(32) PRIMARY KEY, label VARCHAR(120) NOT NULL, dtype VARCHAR(8) NOT NULL)');
const collectionName = name => `browser_${spaceName(name)}`;
let queue = Promise.resolve();
function serial(operation) { const next = queue.catch(() => {}).then(operation); queue = next; return next; }
async function audit(record) { await appendFile(path.join(data, 'audit.jsonl'), `${JSON.stringify({ at: new Date().toISOString(), ...record })}\n`); }
async function getSpace(name) {
  const rows = await client.execute('SELECT name,label,dtype FROM browser_kb_spaces WHERE name = ?', [spaceName(name)]);
  if (!rows?.length) throw new Error('知识库不存在，请先创建');
  return rows[0];
}
async function collection(name) { return client.getCollection({ name: collectionName(name), embeddingFunction: null }); }
async function body(request) {
  const chunks = []; let bytes = 0;
  for await (const chunk of request) { bytes += chunk.length; if (bytes > 8 * 1024 * 1024) throw new Error('请求超过 8 MiB，请分批导入'); chunks.push(chunk); }
  return JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}');
}
function json(response, status, value) { response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' }); response.end(JSON.stringify(value)); }
const mime = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.onnx': 'application/octet-stream', '.wasm': 'application/wasm', '.svg': 'image/svg+xml' };
async function serve(response, directory, pathname) {
  const resolved = path.resolve(directory, `.${pathname}`);
  if (!resolved.startsWith(path.resolve(directory) + path.sep)) { json(response, 404, { error: 'Not found' }); return; }
  let info;
  try { info = await stat(resolved); } catch { json(response, 404, { error: 'File not found; run npm run prepare:model and npm run build' }); return; }
  if (!info.isFile()) { json(response, 404, { error: 'Not found' }); return; }
  response.writeHead(200, { 'Content-Type': mime[path.extname(resolved)] || 'application/octet-stream', 'Content-Length': info.size, 'Cache-Control': pathname.includes('.onnx') || pathname.includes('.wasm') ? 'public, max-age=31536000, immutable' : 'no-cache' });
  const stream = createReadStream(resolved);
  response.on('close', () => stream.destroy());
  stream.on('error', () => response.destroy());
  stream.pipe(response);
}
const server = http.createServer(async (request, response) => {
  response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  response.setHeader('X-Content-Type-Options', 'nosniff');
  response.setHeader('Content-Security-Policy', "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self' blob:; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'self'");
  try {
    const url = new URL(request.url, 'http://localhost');
    if (url.pathname === '/api/shutdown' && request.method === 'POST' && request.headers['x-kb-control'] === 'local-shutdown') {
      json(response, 200, { stopping: true });
      setImmediate(() => stop().catch(error => { console.error(error); process.exitCode = 1; }));
      return;
    }
    if (url.pathname.startsWith('/api/')) {
      const input = request.method === 'POST' ? await body(request) : null;
      const result = await serial(async () => {
        if (url.pathname === '/api/health' && request.method === 'GET') return { ok: true, pid: process.pid, sdk: 'seekdb@1.4.0', databaseVersion: dbVersion, databaseName, mode: 'server', model, inference: 'browser-only' };
        if (url.pathname === '/api/spaces' && request.method === 'GET') {
          const rows = await client.execute('SELECT name,label,dtype FROM browser_kb_spaces ORDER BY name');
          return { spaces: await Promise.all((rows || []).map(async row => ({ ...row, count: await (await collection(row.name)).count() }))) };
        }
        if (url.pathname === '/api/spaces' && request.method === 'POST') {
          const name = spaceName(input.name), dtype = dtypeName(input.dtype);
          if (typeof input.label !== 'string' || !input.label.trim() || input.label.length > 100) throw new Error('请输入 1–100 个字符的知识库名称');
          const existing = await client.execute('SELECT name FROM browser_kb_spaces WHERE name = ?', [name]);
          if (existing?.length) throw new Error('知识库标识已存在');
          await client.getOrCreateCollection({ name: collectionName(name), schema: new Schema({ fulltextIndex: new FulltextIndexConfig('ik', { ik_mode: 'smart' }), vectorIndex: new VectorIndexConfig({ hnsw: { dimension: model.dimension, distance: 'cosine' }, embeddingFunction: null }) }) });
          await client.execute('INSERT INTO browser_kb_spaces(name,label,dtype) VALUES (?,?,?)', [name, input.label.trim(), dtype]);
          await audit({ operation: 'create_collection', name, dtype, model: model.repository, revision: model.revision, dimension: model.dimension, serverEmbeddingFunction: null });
          return { name, label: input.label.trim(), dtype, count: 0 };
        }
        const match = url.pathname.match(/^\/api\/spaces\/([a-z][a-z0-9_]{0,31})(?:\/(documents|search))?$/);
        if (match) {
          const name = match[1], space = await getSpace(name), target = await collection(name);
          if (!match[2] && request.method === 'DELETE') { await client.deleteCollection(collectionName(name)); await client.execute('DELETE FROM browser_kb_spaces WHERE name=?', [name]); await audit({ operation: 'delete_collection', name }); return { deleted: name }; }
          if (match[2] === 'documents' && request.method === 'GET') return { ...space, ...await target.get({ limit: 30, include: ['documents', 'metadatas'] }) };
          if (request.method === 'POST') {
            if (input.dtype !== space.dtype) throw new Error('向量精度与知识库不一致，请切换到该知识库对应的模型精度');
            if (match[2] === 'documents') {
              const rows = documents(input.documents), started = performance.now();
              await target.upsert({ ids: rows.map(x => x.id), documents: rows.map(x => x.text), embeddings: rows.map(x => x.embedding), metadatas: rows.map(x => x.metadata) });
              const writeMs = performance.now() - started, refreshing = performance.now();
              await target.refresh_index();
              const refreshMs = performance.now() - refreshing, count = await target.count();
              await audit({ operation: 'upsert_and_refresh', name, ids: rows.map(x => x.id), dtype: space.dtype, writeMs, refreshMs, count });
              return { processed: rows.length, count, writeMs, refreshMs };
            }
            if (match[2] === 'search') {
              if (typeof input.query !== 'string' || !input.query.trim() || input.query.length > 600) throw new Error('查询应为 1–600 个字符');
              const embedding = vector(input.embedding), limit = Math.min(20, Math.max(1, Number(input.limit) || 5));
              const options = { query: { whereDocument: { $contains: input.query.trim() }, nResults: 40 }, knn: { queryEmbeddings: embedding, nResults: 40 }, rank: { rrf: { rankWindowSize: 40, rankConstant: 60 } }, nResults: limit, include: ['documents', 'metadatas', 'distances'] };
              const started = performance.now();
              const count = await target.count();
              const found = count ? await target.hybridSearch(options) : { ids: [[]], documents: [[]], metadatas: [[]], distances: [[]] };
              const searchMs = performance.now() - started;
              const hits = (found.ids[0] || []).map((id, index) => ({ id, text: found.documents?.[0]?.[index], metadata: found.metadatas?.[0]?.[index], score: found.distances?.[0]?.[index] }));
              await audit({ operation: 'native_hybrid_search', name, query: input.query, dtype: space.dtype, vectorDimension: embedding.length, sdkMethod: 'Collection.hybridSearch', databaseFusion: 'DBMS_HYBRID_SEARCH', rank: options.rank, ids: hits.map(x => x.id), sourceIds: hits.map(x => x.metadata?.sourceId), searchMs });
              return { hits, searchMs, count, fusion: 'seekdb native RRF' };
            }
          }
        }
        throw new Error('API 不存在或请求方法不正确');
      });
      json(response, 200, result); return;
    }
    if (url.pathname === '/model.json') { json(response, 200, model); return; }
    if (url.pathname === '/sample.json') { await serve(response, root, '/sample.json'); return; }
    if (url.pathname.startsWith('/models/')) { await serve(response, path.join(root, '.cache/models'), decodeURIComponent(url.pathname.slice(7))); return; }
    if (url.pathname.startsWith('/ort/')) { await serve(response, ortDirectory, decodeURIComponent(url.pathname.slice(4))); return; }
    await serve(response, path.join(root, 'dist'), url.pathname === '/' ? '/index.html' : decodeURIComponent(url.pathname));
  } catch (error) { if (!response.headersSent) json(response, 400, { error: error.message }); else response.destroy(); }
});
const port = Number(process.env.PORT || 18776);
server.listen(port, process.env.HOST || '127.0.0.1', () => console.log(`Near Knowledge: http://127.0.0.1:${port} — seekdb ready, inference stays in the browser`));
let stopping = false;
async function stop() {
  if (stopping) return;
  stopping = true;
  await audit({ operation: 'shutdown_begin' });
  const closed = new Promise(resolve => server.close(resolve));
  await queue.catch(() => {});
  // Model download/cache streams may outlive inference. After all SQL work
  // drains, close those HTTP sockets too instead of waiting on browser caches.
  server.closeAllConnections();
  await closed;
  await client.close();
  await audit({ operation: 'shutdown_complete' });
  process.exitCode = 0;
}
process.on('SIGTERM', stop); process.on('SIGINT', stop);
