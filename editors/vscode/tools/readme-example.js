// readme-example.js — renders the README's example program as light and dark SVG images, coloured by this
// extension's own grammar (syntaxes/bezel.tmLanguage.json), because GitHub does not highlight Bezel blocks.
// The block is the ```bezel fence after the `<!-- readme-example` marker in the repository's README.md.
// Run from editors/vscode after `npm install --no-save vscode-textmate@9 vscode-oniguruma@2`:
//   node tools/readme-example.js          # rewrites docs/images/readme-example-{light,dark}.svg
//   node tools/readme-example.js --check  # fails if the images are out of date
const fs = require('fs'), path = require('path'), vt = require('vscode-textmate'), og = require('vscode-oniguruma');
const GRAMMAR = path.join(__dirname, '..', 'syntaxes', 'bezel.tmLanguage.json');
const THEMES = {
  light: { bg: '#f6f8fa', fg: '#1f2328', keyword: '#cf222e', type: '#8250df', string: '#0a3069', comment: '#59636e', constant: '#0550ae' },
  dark:  { bg: '#151b23', fg: '#f0f6fc', keyword: '#ff7b72', type: '#d2a8ff', string: '#a5d6ff', comment: '#9198a1', constant: '#79c0ff' },
};
function role(scopes) {                       // innermost scope decides
  for (let i = scopes.length - 1; i >= 0; i--) {
    const s = scopes[i];
    if (s.startsWith('meta.interpolation')) return 'fg';
    if (s.startsWith('punctuation.section.interpolation')) return 'string';
    if (s.startsWith('comment')) return 'comment';
    if (s.startsWith('string') || s.startsWith('constant.character')) return 'string';
    if (s.startsWith('constant') || s.startsWith('variable.language')) return 'constant';
    if (/^keyword\.operator\.(arrow|assignment|comparison|null|bezel)/.test(s)) return 'fg';
    if (s.startsWith('keyword') || s.startsWith('storage')) return 'keyword';
    if (s.startsWith('entity.name')) return 'type';
  }
  return 'fg';
}
const esc = t => t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
(async () => {
  const root = path.join(__dirname, '..', '..', '..');
  const check = process.argv.includes('--check');
  const m = fs.readFileSync(path.join(root, 'README.md'), 'utf8').match(/<!-- readme-example[^\n]*\n```bezel\n([\s\S]*?)\n```/);
  if (!m) { console.error('readme-example: no marked ```bezel block in README.md'); process.exit(2); }
  const code = m[1];
  const outDir = path.join(root, 'docs', 'images');
  let stale = 0;
  await og.loadWASM(fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer);
  const reg = new vt.Registry({
    onigLib: Promise.resolve({ createOnigScanner: s => new og.OnigScanner(s), createOnigString: s => new og.OnigString(s) }),
    loadGrammar: async () => vt.parseRawGrammar(fs.readFileSync(GRAMMAR, 'utf8'), GRAMMAR),
  });
  const g = await reg.loadGrammar('source.bezel');
  let st = vt.INITIAL; const lines = [];
  for (const line of code.split('\n')) {
    const r = g.tokenizeLine(line, st); st = r.ruleStack;
    lines.push(r.tokens.map(t => ({ text: line.slice(t.startIndex, t.endIndex), role: role(t.scopes) })));
  }
  const FS = 13.6, LH = 20, CW = 8.2, PAD = 16;
  const cols = Math.max(...code.split('\n').map(l => l.length));
  const W = Math.ceil(cols * CW + 2 * PAD), H = lines.length * LH + 2 * PAD - 4;
  for (const [name, th] of Object.entries(THEMES)) {
    let out = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" role="img" aria-label="Bezel code example">\n`;
    out += `<rect width="100%" height="100%" rx="6" fill="${th.bg}"/>\n`;
    out += `<text font-family="ui-monospace, SFMono-Regular, 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace" font-size="${FS}" xml:space="preserve" fill="${th.fg}">\n`;
    lines.forEach((toks, i) => {
      out += `<tspan x="${PAD}" y="${PAD + 12 + i * LH}">` + toks.map(t => t.role === 'fg' ? esc(t.text) : `<tspan fill="${th[t.role]}">${esc(t.text)}</tspan>`).join('') + `</tspan>\n`;
    });
    out += `</text>\n</svg>\n`;
    const file = path.join(outDir, `readme-example-${name}.svg`);
    if (check) { if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8') !== out) { console.error(`stale: ${file}`); stale++; } }
    else fs.writeFileSync(file, out);
  }
  if (stale) process.exit(1);
  console.log(`readme-example: ${check ? 'up to date' : 'wrote'} (${lines.length} lines, ${W}x${H})`);
})();
