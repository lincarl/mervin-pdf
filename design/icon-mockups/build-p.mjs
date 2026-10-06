import { mkdir, writeFile, copyFile } from 'node:fs/promises';

// Build paired P concepts without changing the M studies or application icons.
const root = new URL('./p/', import.meta.url);
await mkdir(new URL('assets/', root), { recursive: true });
const gradient = (id, a, b) => `<linearGradient id="${id}" x2="1" y2="1"><stop stop-color="${a}"/><stop offset="1" stop-color="${b}"/></linearGradient>`;
const square = (fill, r = 44) => `<rect width="256" height="256" rx="${r}" fill="${fill}"/>`;
const classic = 'M86 181V81H132C164 81 181 94 181 114C181 134 163 147 133 147H111V181ZM111 102V126H131C148 126 156 122 156 114C156 106 148 102 131 102Z';
const geometric = 'M75 191V65H139C172 65 191 81 191 108C191 135 172 151 139 151H109V191ZM109 93V123H137C151 123 158 118 158 108C158 98 151 93 137 93Z';
const angular = 'M79 192V64H146L183 91V122L153 149H111V192ZM111 92V121H140L152 111V103L140 92Z';
const p = (d = geometric, fill = '#fff', transform = '') => `<path d="${d}" fill="${fill}" fill-rule="evenodd" transform="${transform}"/>`;
const blue = gradient('blue', '#2aa7e0', '#0a4d8c');
const page = '<path d="M55 9H157L215 67V226Q215 247 194 247H55Q34 247 34 226V30Q34 9 55 9Z"';
const concepts = [
  ['01', 'familiar', 'Familiar blue', 'The current blue gradient and proportions, with a drawn P.', blue,
    square('url(#blue)', 42) + p(classic)],
  ['02', 'bold', 'Bold geometric', 'A broad P with a generous counter and a strong vertical stem.', gradient('blue', '#258fdd', '#0753ae'),
    square('url(#blue)', 44) + p()],
  ['03', 'compact', 'Compact', 'A tall angular P and tighter corners for a technical feel.', '',
    square('#1169bd', 28) + p(angular)],
  ['04', 'rounded', 'Soft line', 'A rounded stroke gives the P a gentler shape.', gradient('blue', '#37aee0', '#176aaa'),
    square('url(#blue)', 60) + '<path d="M85 184V78H132Q177 78 177 112Q177 146 132 146H85" fill="none" stroke="#fff" stroke-width="29" stroke-linecap="round" stroke-linejoin="round"/>'],
  ['05', 'paper', 'White page', 'A folded white page with a blue P makes the document link clear.', '',
    page + ' fill="#f8fbff" stroke="#b9cede" stroke-width="3"/><path d="M157 9V48Q157 67 176 67H215Z" fill="#c5dced"/>' + p(geometric, '#1265b3', 'translate(24 44) scale(.8)')],
  ['06', 'blue-page', 'Blue page', 'The same document silhouette in the existing blue palette.', blue,
    page + ' fill="url(#blue)"/><path d="M157 9V48Q157 67 176 67H215Z" fill="#91d9f5"/>' + p(geometric, '#fff', 'translate(24 44) scale(.8)')],
  ['07', 'folded', 'Folded ribbon', 'An angular P with folded strips and a small amount of depth.', gradient('navy', '#163f64', '#0b223c'),
    square('url(#navy)', 44) + p('M74 192V64H147L190 97V123L160 151H110V192ZM110 94V122H144L158 110L144 94Z', '#f4fbff') + '<path d="M74 64L110 94V192H74Z" fill="#a7dbf5"/><path d="M74 64H147L177 87H103Z" fill="#d3effc"/>'],
  ['08', 'stack', 'Page stack', 'Two offset sheets and a bold P suggest work with documents.', blue,
    '<rect x="26" y="12" width="166" height="220" rx="23" fill="#a2d6f0"/><rect x="54" y="36" width="176" height="210" rx="23" fill="url(#blue)"/>' + p(geometric, '#fff', 'translate(47 41) scale(.74)')],
  ['09', 'book', 'Open book', 'A clear P sits above a small open book with a central spine.', gradient('blue', '#248ed2', '#164c97'),
    square('url(#blue)', 44) + p(geometric, '#fff', 'translate(0 -19)') + '<path d="M53 183Q89 177 128 192Q167 177 203 183V204Q166 198 128 216Q90 198 53 204Z" fill="#fff"/><path d="M128 192V216" stroke="#8bc6ea" stroke-width="6"/>'],
  ['10', 'mono', 'Monochrome', 'A white P on charcoal, with no gradient or extra detail.', '',
    square('#232a32', 44) + '<rect x="1.5" y="1.5" width="253" height="253" rx="43" fill="none" stroke="#596574" stroke-width="3"/>' + p()],
  ['11', 'reversed', 'White tile', 'A blue P on white, with a subtle edge for light desktops.', '',
    square('#f6f9fc', 44) + '<rect x="1.5" y="1.5" width="253" height="253" rx="43" fill="none" stroke="#c2d4e0" stroke-width="3"/>' + p(geometric, '#1568b7')],
  ['12', 'teal', 'Open teal', 'A freestanding P with no tile, using a blue-to-teal gradient.', gradient('teal', '#42c5dd', '#118b94'),
    p('M51 220V36H144C190 36 217 61 217 99C217 138 190 163 144 163H98V220ZM98 78V121H140C161 121 171 114 171 99C171 85 161 78 140 78Z', 'url(#teal)')],
];

const manifest = [];
for (const [id, slug, name, description, defs, art] of concepts) {
  const stem = `${id}-${slug}`;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256" role="img" aria-labelledby="title"><title id="title">${name}, Mervin P icon concept ${id}</title><defs>${defs}</defs>${art}</svg>\n`;
  await writeFile(new URL(`assets/${stem}.svg`, root), svg);
  manifest.push({ id, stem, name, description });
}
await writeFile(new URL('concepts.json', root), JSON.stringify(manifest, null, 2) + '\n');
await copyFile(new URL('../../resources/icons/mervin-icon.png', import.meta.url), new URL('assets/current-p.png', root));
console.log(`Built ${manifest.length} SVG P icon concepts.`);
