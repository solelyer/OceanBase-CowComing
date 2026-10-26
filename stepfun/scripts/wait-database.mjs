import { SeekdbClient } from 'seekdb';

const deadline = Date.now() + 120000;
let lastError;
while (Date.now() < deadline) {
  const client = new SeekdbClient({ host: process.env.SEEKDB_HOST || '127.0.0.1', port: Number(process.env.SEEKDB_PORT || 12891), user: process.env.SEEKDB_USER || 'root', password: process.env.SEEKDB_PASSWORD || '', database: 'test', queryTimeout: 5000 });
  try {
    const version = await client.execute('SELECT VERSION() AS version');
    console.log('seekdb SQL ready:', version);
    await client.close(); process.exit(0);
  } catch (error) { lastError = error; await client.close().catch(() => {}); }
  await new Promise(resolve => setTimeout(resolve, 1000));
}
throw new Error(`seekdb did not become ready: ${lastError?.message}`);
