// Run through agent-browser eval --stdin after opening this gallery.
// Chromium rasterizes each SVG onto a transparent canvas at its output size.
(async () => {
  const concepts = await fetch('concepts.json').then(response => response.json());
  const output = [];
  for (const concept of concepts) {
    const image = new Image();
    image.src = `assets/${concept.stem}.svg`;
    await image.decode();
    for (const size of [256, 512]) {
      const canvas = document.createElement('canvas');
      canvas.width = size;
      canvas.height = size;
      canvas.getContext('2d').drawImage(image, 0, 0, size, size);
      output.push({ name: `${concept.stem}-${size}.png`, data: canvas.toDataURL('image/png').split(',')[1] });
    }
  }
  return output;
})();
