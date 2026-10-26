import { pipeline, env } from '@huggingface/transformers';

env.allowRemoteModels = false;
env.allowLocalModels = true;
env.localModelPath = '/models/';
env.backends.onnx.wasm.wasmPaths = '/ort/';
env.backends.onnx.wasm.numThreads = self.crossOriginIsolated ? Math.min(4, navigator.hardwareConcurrency || 1) : 1;
let extractor, current, model;
async function initialize({ device = 'auto', dtype = 'q8' }) {
  if (!['auto', 'webgpu', 'wasm'].includes(device) || !['q8', 'fp32'].includes(dtype)) throw new Error('Invalid inference configuration');
  if (extractor && current.requestedDevice === device && current.dtype === dtype) return current;
  if (extractor) { await extractor.dispose(); extractor = null; }
  model ||= await (await fetch('/model.json')).json();
  const started = performance.now();
  let adapter = null, info = null, reason = '';
  if (device !== 'wasm') {
    try { adapter = await navigator.gpu?.requestAdapter(); } catch (error) { reason = error.message; }
    if (adapter) {
      const value = adapter.info || {};
      info = { vendor: value.vendor || '', architecture: value.architecture || '', device: value.device || '', description: value.description || '', features: [...adapter.features] };
    } else reason ||= '浏览器没有可用的 WebGPU 适配器';
  }
  const load = async backend => pipeline('feature-extraction', model.localName, { device: backend, dtype, local_files_only: true, progress_callback: progress => postMessage({ progress: { status: progress.status, file: progress.file, loaded: progress.loaded, total: progress.total, progress: progress.progress } }) });
  let backend = device === 'wasm' ? 'wasm' : adapter ? 'webgpu' : 'wasm';
  if (device === 'webgpu' && !adapter) throw new Error(reason);
  try { extractor = await load(backend); }
  catch (error) {
    if (backend !== 'webgpu' || device === 'webgpu') throw error;
    reason = `WebGPU 初始化失败，已切换 WASM：${error.message}`;
    backend = 'wasm'; extractor = await load('wasm');
  }
  current = { backend, dtype, requestedDevice: device, adapter: info, fallbackReason: reason, loadMs: performance.now() - started, model: model.repository, revision: model.revision, threads: env.backends.onnx.wasm.numThreads };
  return current;
}
async function embed({ texts, batchSize = 8, query = false }) {
  if (!extractor) throw new Error('请先加载模型');
  if (!Array.isArray(texts) || !texts.length || texts.some(x => typeof x !== 'string' || !x.trim())) throw new Error('请提供非空文本');
  batchSize = Math.min(16, Math.max(1, batchSize));
  const started = performance.now(), embeddings = [];
  for (let offset = 0; offset < texts.length; offset += batchSize) {
    const batch = texts.slice(offset, offset + batchSize).map(text => query ? `为这个句子生成表示以用于检索相关文章：${text}` : text);
    const output = await extractor(batch, { pooling: model.pooling, normalize: true, truncation: true, max_length: 256 });
    embeddings.push(...output.tolist());
    postMessage({ progress: { status: 'embedding', done: embeddings.length, total: texts.length } });
  }
  return { embeddings, embeddingMs: performance.now() - started, ...current };
}
let pending = Promise.resolve();
self.onmessage = event => {
  const { id, operation, input } = event.data;
  pending = pending.catch(() => {}).then(async () => {
    try { postMessage({ id, result: operation === 'initialize' ? await initialize(input) : operation === 'embed' ? await embed(input) : (() => { throw new Error('Unknown operation'); })() }); }
    catch (error) { postMessage({ id, error: error.message }); }
  });
};
