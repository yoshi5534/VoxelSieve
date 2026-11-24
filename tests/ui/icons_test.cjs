// Unit tests of the icons of the UI (resources/ui/icons.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { ICON_SHAPES } = require(path.join(__dirname, '../../resources/ui/icons.js'));

// Every icon is a few SVG shapes, nothing that runs or loads.
for (const [name, shapes] of Object.entries(ICON_SHAPES)) {
  assert.ok(shapes.length > 0, name);
  for (const tag of shapes.matchAll(/<([a-z]+)/g)) {
    assert.ok(['path', 'circle', 'rect', 'line', 'polyline', 'polygon', 'ellipse'].includes(tag[1]),
      `${name} has a <${tag[1]}>`);
  }
  assert.ok(!/on[a-z]+=|href|script/i.test(shapes), name);
}

// Every icon the UI asks for by a literal name exists.
const ui = path.join(__dirname, '../../resources/ui');
for (const file of fs.readdirSync(ui).filter((f) => f.endsWith('.js') || f.endsWith('.html'))) {
  const text = fs.readFileSync(path.join(ui, file), 'utf8');
  for (const use of text.matchAll(/\bicon\('([a-z0-9-]+)'|data-icon="([a-z0-9-]+)"|icon: '([a-z0-9-]+)'/g)) {
    const name = use[1] ?? use[2] ?? use[3];
    assert.ok(name in ICON_SHAPES, `${file} uses the unknown icon ${name}`);
  }
}

console.log('icons tests passed');
