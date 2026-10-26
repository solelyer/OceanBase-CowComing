import { createHash } from 'node:crypto';

export const DIMENSION = 512;
export const DTYPES = ['q8', 'fp32'];
export function spaceName(value) {
  if (typeof value !== 'string' || !/^[a-z][a-z0-9_]{0,31}$/.test(value)) throw new Error('知识库标识需为 1–32 位小写字母、数字或下划线，并以字母开头');
  return value;
}
export function dtypeName(value) {
  if (!DTYPES.includes(value)) throw new Error('不支持的模型精度');
  return value;
}
export function vector(value) {
  if (!Array.isArray(value) || value.length !== DIMENSION || value.some(x => typeof x !== 'number' || !Number.isFinite(x))) throw new Error(`向量必须包含 ${DIMENSION} 个有限数值`);
  const norm = Math.hypot(...value);
  if (Math.abs(norm - 1) > 0.02) throw new Error('请使用浏览器归一化后的非零向量');
  return value;
}
export function documents(value) {
  if (!Array.isArray(value) || !value.length || value.length > 100) throw new Error('每批应提交 1–100 个文本段落');
  return value.map(row => {
    if (typeof row.text !== 'string' || !row.text.trim() || row.text.length > 2000) throw new Error('段落正文应为 1–2000 个字符');
    if (typeof row.sourceId !== 'string' || !row.sourceId || row.sourceId.length > 180) throw new Error('缺少有效来源标识');
    const source = typeof row.source === 'string' ? row.source.slice(0, 180) : row.sourceId;
    return { id: createHash('sha256').update(`${row.sourceId}\0${row.text}`).digest('hex'), text: row.text, embedding: vector(row.embedding), metadata: { source, sourceId: row.sourceId, part: Number.isInteger(row.part) ? row.part : 1 } };
  });
}
