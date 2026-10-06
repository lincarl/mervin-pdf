import { mkdir, writeFile, copyFile } from 'node:fs/promises';

// Build standalone vector concepts without touching the application icons.
const root = new URL('./', import.meta.url);
await mkdir(new URL('assets/', root), { recursive: true });
const gradient = (id, a, b) => `<linearGradient id="${id}" x2="1" y2="1"><stop stop-color="${a}"/><stop offset="1" stop-color="${b}"/></linearGradient>`;
const square = (fill, r = 44) => `<rect width="256" height="256" rx="${r}" fill="${fill}"/>`;
const classic = 'M65 181V81H91L128 148L165 81H191V181H167V121L139 173H117L89 121V181Z';
const geometric = 'M55 191V65H86L128 128L170 65H201V191H168V123L137 169H119L88 123V191Z';
const m = (d = geometric, fill = '#fff', transform = '') => `<path d="${d}" fill="${fill}" transform="${transform}"/>`;
const blue = gradient('blue', '#2aa7e0', '#0a4d8c');
const concepts = [
  ['01', 'familiar', 'Familiar blue', 'The current blue gradient and proportions, with an M.', blue,
    square('url(#blue)', 42) + m(classic)],
  ['02', 'bold', 'Bold geometric', 'A larger custom M with broad stems and an open center.', gradient('blue', '#258fdd', '#0753ae'),
    square('url(#blue)', 44) + m()],
  ['03', 'compact', 'Compact', 'A tall, narrow M and tighter corners for a technical feel.', '',
    square('#1169bd', 28) + m('M70 192V64H95L128 126L161 64H186V192H160V116L137 160H119L96 116V192Z')],
  ['04', 'rounded', 'Soft line', 'One continuous rounded stroke, with a gentler shape.', gradient('blue', '#37aee0', '#176aaa'),
    square('url(#blue)', 60) + '<path d="M66 184V77L128 147L190 77V184" fill="none" stroke="#fff" stroke-width="29" stroke-linecap="round" stroke-linejoin="round"/>'],
  ['05', 'paper', 'White page', 'A folded white page with a blue M makes the document link clear.', '',
    '<path d="M55 9H157L215 67V226Q215 247 194 247H55Q34 247 34 226V30Q34 9 55 9Z" fill="#f8fbff" stroke="#b9cede" stroke-width="3"/><path d="M157 9V48Q157 67 176 67H215Z" fill="#c5dced"/>' + m(geometric, '#1265b3', 'translate(24 44) scale(.8)')],
  ['06', 'blue-page', 'Blue page', 'The same document silhouette in the existing blue palette.', blue,
    '<path d="M55 9H157L215 67V226Q215 247 194 247H55Q34 247 34 226V30Q34 9 55 9Z" fill="url(#blue)"/><path d="M157 9V48Q157 67 176 67H215Z" fill="#91d9f5"/>' + m(geometric, '#fff', 'translate(24 44) scale(.8)')],
  ['07', 'folded', 'Folded ribbon', 'Folded strips form an M with a small amount of depth.', gradient('navy', '#163f64', '#0b223c'),
    square('url(#navy)', 44) + '<path d="M53 190V65H85L128 125L171 65H203V190H169V120L128 175L87 120V190Z" fill="#f4fbff"/><path d="M85 65L128 125V175L53 65Z" fill="#a7dbf5"/><path d="M171 65H203L128 175V125Z" fill="#d3effc"/>'],
  ['08', 'stack', 'Page stack', 'Two offset sheets suggest everyday work with documents.', blue,
    '<rect x="26" y="12" width="166" height="220" rx="23" fill="#a2d6f0"/><rect x="54" y="36" width="176" height="210" rx="23" fill="url(#blue)"/>' + m(geometric, '#fff', 'translate(47 41) scale(.74)')],
  ['09', 'book', 'Open book', 'The M doubles as an open book, with a visible central spine.', gradient('blue', '#248ed2', '#164c97'),
    square('url(#blue)', 44) + '<path d="M48 69L80 62L128 118L176 62L208 69V192L175 183V112L128 166L81 112V183L48 192Z" fill="#fff"/><path d="M128 118V166" stroke="#8bc6ea" stroke-width="6"/>'],
  ['10', 'mono', 'Monochrome', 'A white M on charcoal, with no gradient or extra detail.', '',
    square('#232a32', 44) + '<rect x="1.5" y="1.5" width="253" height="253" rx="43" fill="none" stroke="#596574" stroke-width="3"/>' + m()],
  ['11', 'reversed', 'White tile', 'A blue M on white, with a subtle edge for light desktops.', '',
    square('#f6f9fc', 44) + '<rect x="1.5" y="1.5" width="253" height="253" rx="43" fill="none" stroke="#c2d4e0" stroke-width="3"/>' + m(geometric, '#1568b7')],
  ['12', 'teal', 'Open teal', 'A freestanding M with no tile, using a blue-to-teal gradient.', gradient('teal', '#42c5dd', '#118b94'),
    m('M20 220V36H60L128 132L196 36H236V220H193V110L141 184H115L63 110V220Z', 'url(#teal)')],
];

const manifest = [];
for (const [id, slug, name, description, defs, art] of concepts) {
  const stem = `${id}-${slug}`;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256" role="img" aria-labelledby="title"><title id="title">${name}, Mervin M icon concept ${id}</title><defs>${defs}</defs>${art}</svg>\n`;
  await writeFile(new URL(`assets/${stem}.svg`, root), svg);
  manifest.push({ id, stem, name, description });
}
await writeFile(new URL('concepts.json', root), JSON.stringify(manifest, null, 2) + '\n');
await copyFile(new URL('../../resources/icons/mervin-icon.png', root), new URL('assets/current-p.png', root));
console.log(`Built ${manifest.length} SVG icon concepts.`);
