import { mkdir, writeFile, copyFile } from 'node:fs/promises';
import documents from './objects/concepts-a.mjs';
import reading from './objects/concepts-b.mjs';
import tools from './objects/concepts-c.mjs';

// Keep the pictorial studies separate from the application and letter assets.
const root = new URL('./objects/', import.meta.url);
await mkdir(new URL('assets/', root), { recursive: true });
const concepts = [...documents, ...reading, ...tools];
const manifest = [];
for (const { id, stem, name, description, defs, art } of concepts) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256" role="img" aria-labelledby="title"><title id="title">${name}, Mervin image icon concept ${id}</title><defs>${defs}</defs>${art}</svg>\n`;
  await writeFile(new URL(`assets/${stem}.svg`, root), svg);
  manifest.push({ id, stem, name, description });
}
await writeFile(new URL('concepts.json', root), JSON.stringify(manifest, null, 2) + '\n');
await copyFile(new URL('../../resources/icons/mervin-icon.png', import.meta.url), new URL('assets/current-p.png', root));
console.log(`Built ${manifest.length} image icon concepts.`);
