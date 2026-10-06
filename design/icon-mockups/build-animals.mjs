import { mkdir, writeFile, copyFile } from 'node:fs/promises';
import first from './animals/concepts-a.mjs';
import second from './animals/concepts-b.mjs';
import third from './animals/concepts-c.mjs';
import fourth from './animals/concepts-d.mjs';

// Animal studies remain separate from the application's packaged icons.
const root = new URL('./animals/', import.meta.url);
await mkdir(new URL('assets/', root), { recursive: true });
const concepts = [...first, ...second, ...third, ...fourth];
const manifest = [];
for (const { id, stem, name, description, defs, art } of concepts) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256" role="img" aria-labelledby="title"><title id="title">${name}, Mervin animal icon concept ${id}</title><defs>${defs}</defs>${art}</svg>\n`;
  await writeFile(new URL(`assets/${stem}.svg`, root), svg);
  manifest.push({ id, stem, name, description });
}
await writeFile(new URL('concepts.json', root), JSON.stringify(manifest, null, 2) + '\n');
await copyFile(new URL('../../resources/icons/mervin-icon.png', import.meta.url), new URL('assets/current-p.png', root));
console.log(`Built ${manifest.length} animal icon concepts.`);
