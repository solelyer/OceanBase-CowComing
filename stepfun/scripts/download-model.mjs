import { readFile, mkdir, stat, rename, rm, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { spawn } from 'node:child_process';

const root = fileURLToPath(new URL('..', import.meta.url));
const model = JSON.parse(await readFile(path.join(root, 'model.json'), 'utf8'));
const base = process.env.MODEL_DOWNLOAD_ORIGIN || 'https://huggingface.co';
if (!['https://huggingface.co', 'https://hf-mirror.com'].includes(base)) throw new Error('Unsupported model download origin');
const directory = path.join(root, '.cache/models', model.localName);
const records = [];
for (const [name, expected] of Object.entries(model.files)) {
  const target = path.join(directory, name);
  await mkdir(path.dirname(target), { recursive: true });
  async function verify(file) {
    try {
      if ((await stat(file)).size !== expected.bytes) return false;
      return !expected.sha256 || createHash('sha256').update(await readFile(file)).digest('hex') === expected.sha256;
    } catch { return false; }
  }
  const started = performance.now();
  const cached = await verify(target);
  if (!cached) {
    const temporary = `${target}.partial`;
    const url = `${base}/${model.repository}/resolve/${model.revision}/${name}?download=true`;
    console.log(`Downloading ${name} (${expected.bytes} bytes)`);
    await new Promise((resolve, reject) => {
      const child = spawn('curl', ['--fail', '--location', '--silent', '--show-error', '--retry', '2', '--max-time', '600', url, '--output', temporary], { stdio: 'inherit' });
      child.on('error', reject);
      child.on('exit', code => code === 0 ? resolve() : reject(new Error(`curl exited ${code}: ${name}`)));
    });
    if (!await verify(temporary)) { await rm(temporary, { force: true }); throw new Error(`Model integrity check failed: ${name}`); }
    await rename(temporary, target);
  }
  records.push({ name, bytes: expected.bytes, sha256: createHash('sha256').update(await readFile(target)).digest('hex'), cached, elapsedMs: performance.now() - started });
}
await writeFile(path.join(root, '.cache/model-download.json'), JSON.stringify({ repository: model.repository, revision: model.revision, files: records }, null, 2));
console.log(`Model files verified in ${directory}`);
