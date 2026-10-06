const grid = document.querySelector('#grid');
const dialog = document.querySelector('dialog');
const letter = document.body.dataset.letter;
let concepts = [];
let selected = 0;

function sizePreview(concept, size) {
  return `<span class="size"><img src="assets/${concept.stem}.svg" width="${size}" height="${size}" alt=""><small>${size}</small></span>`;
}

// All contexts reference the same SVG so the large and small previews agree.
function showConcept(index) {
  selected = (index + concepts.length) % concepts.length;
  const concept = concepts[selected];
  document.querySelector('#detail-number').textContent = `Concept ${letter}${concept.id} / ${concepts.length}`;
  document.querySelector('#detail-title').textContent = concept.name;
  document.querySelector('#detail-description').textContent = concept.description;
  const hero = document.querySelector('#hero');
  hero.src = `assets/${concept.stem}.svg`;
  hero.alt = concept.name;
  document.querySelector('#size-strip').innerHTML = [16,24,32,48].map(size => sizePreview(concept,size)).join('');
  document.querySelectorAll('.context-icon').forEach(img => { img.src = hero.src; });
  document.querySelector('#download-svg').href = `assets/${concept.stem}.svg`;
  document.querySelector('#download-png').href = `assets/${concept.stem}-512.png`;
  if (!dialog.open) dialog.showModal();
}

document.querySelector('.close').addEventListener('click', () => dialog.close());
document.querySelector('#previous').addEventListener('click', () => showConcept(selected - 1));
document.querySelector('#next').addEventListener('click', () => showConcept(selected + 1));
dialog.addEventListener('click', event => {
  const rect = dialog.getBoundingClientRect();
  if (event.target === dialog && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom)) dialog.close();
});
document.querySelectorAll('[data-theme]').forEach(button => button.addEventListener('click', () => {
  document.body.classList.toggle('light', button.dataset.theme === 'light');
  document.querySelectorAll('[data-theme]').forEach(item => item.setAttribute('aria-pressed', String(item === button)));
}));

fetch('concepts.json').then(response => {
  if (!response.ok) throw new Error('Could not load the icon concepts.');
  return response.json();
}).then(data => {
  concepts = data;
  grid.innerHTML = concepts.map(concept => `<button class="concept" type="button" data-id="${concept.id}" aria-label="Preview ${letter}${concept.id}, ${concept.name}"><span class="concept-heading"><span class="number">${letter}${concept.id}</span>${concept.name}</span><img class="large-icon" src="assets/${concept.stem}.svg" alt="" width="104" height="104"><span class="sizes">${[16,24,32].map(size => sizePreview(concept,size)).join('')}</span></button>`).join('');
  grid.querySelectorAll('.concept').forEach((button, index) => button.addEventListener('click', () => showConcept(index)));
}).catch(error => { grid.textContent = error.message; });
