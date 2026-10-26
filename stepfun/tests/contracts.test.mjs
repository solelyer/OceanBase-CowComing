import test from 'node:test';
import assert from 'node:assert/strict';
import { documents, spaceName, vector } from '../lib/contracts.mjs';

test('reject invalid and unnormalized browser vectors before writing', () => {
  assert.throws(() => vector([1]), /512/);
  assert.throws(() => vector(Array(512).fill(NaN)), /有限/);
  assert.throws(() => vector(Array(512).fill(0)), /归一化/);
  assert.throws(() => vector(Array(512).fill('0')), /有限/);
  assert.equal(vector([1, ...Array(511).fill(0)]).length, 512);
});
test('stable identities deduplicate identical content and retain source identity', () => {
  const row = { sourceId: 'note.md', text: 'original text', embedding: [1, ...Array(511).fill(0)] };
  assert.equal(documents([row])[0].id, documents([{ ...row }])[0].id);
  assert.notEqual(documents([row])[0].id, documents([{ ...row, sourceId: 'other.md' }])[0].id);
  assert.notEqual(documents([row])[0].id, documents([{ ...row, text: 'updated text' }])[0].id);
});
test('collection identifiers cannot escape their database namespace', () => {
  for (const name of ['../x', 'notes;DROP TABLE x', 'A', 'a'.repeat(33), '', '_x']) assert.throws(() => spaceName(name));
  assert.equal(spaceName('my_notes_2'), 'my_notes_2');
});
