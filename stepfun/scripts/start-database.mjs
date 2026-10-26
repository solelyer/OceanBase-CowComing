import { spawn } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = fileURLToPath(new URL('..', import.meta.url));
const directory = path.resolve(process.env.SEEKDB_DATA_DIR || path.join(root, '.data/seekdb-server'));
await mkdir(directory, { recursive: true });
const port = String(process.env.SEEKDB_PORT || 12891);
const image = 'oceanbase/seekdb@sha256:4db4961869d554313520d6081fca29265ba6da6db7313311bd8acbed771a956b';
const binary = process.env.SEEKDB_SERVER_BIN;
const command = binary || 'docker';
const args = binary ? ['--nodaemon', '--base-dir', directory, '--port', port, '--log-level', 'INFO', '--parameter', 'memory_budget=4G', '--parameter', 'cpu_count=4', '--parameter', 'datafile_size=2G', '--parameter', 'datafile_maxsize=4G', '--parameter', 'log_disk_size=2G'] : ['run', '--rm', '--name', 'near-knowledge-seekdb', '-p', `127.0.0.1:${port}:2881`, '-e', 'CPU_COUNT=4', '-e', 'MEMORY_LIMIT=4G', '-e', 'DATAFILE_SIZE=2G', '-e', 'DATAFILE_MAXSIZE=4G', '-v', `${directory}:/var/lib/seekdb`, image];
console.log(`Starting local seekdb ${binary ? 'binary' : 'official pinned Docker image'} on ${port}; data: ${directory}`);
const child = spawn(command, args, { cwd: directory, stdio: 'inherit' });
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
child.on('error', error => { console.error(error.message); process.exitCode = 1; });
child.on('exit', (code, signal) => { process.exitCode = code ?? (signal ? 1 : 0); });
