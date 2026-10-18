// Tokenizes every ```bezel block in the normative docs with the generated
// TextMate grammar (vscode-textmate + Oniguruma, the engine VS Code uses) and
// fails if a block ends inside a string or comment, which is the usual sign of
// a grammar defect. Usage (from editors/vscode):
//   npm install --no-save vscode-textmate@9 vscode-oniguruma@2
//   node test/tokenize.js
const fs = require('fs');
const path = require('path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

const root = path.resolve(__dirname, '..', '..', '..');
const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
const lib = oniguruma.loadWASM(wasm).then(() => ({
  createOnigScanner: (p) => new oniguruma.OnigScanner(p),
  createOnigString: (s) => new oniguruma.OnigString(s),
}));
const registry = new vsctm.Registry({
  onigLib: lib,
  loadGrammar: async () => vsctm.parseRawGrammar(
    fs.readFileSync(path.join(__dirname, '..', 'syntaxes', 'bezel.tmLanguage.json'), 'utf8'),
    'bezel.tmLanguage.json'),
});

(async () => {
  const grammar = await registry.loadGrammar('source.bezel');
  const docs = fs.readdirSync(path.join(root, 'docs')).filter((f) => f.endsWith('.md'));
  let blocks = 0, lines = 0, bad = [];
  const scopes = new Map();
  for (const f of docs) {
    const text = fs.readFileSync(path.join(root, 'docs', f), 'utf8');
    const re = /^```bezel\n([\s\S]*?)^```/gm;
    let m;
    while ((m = re.exec(text))) {
      blocks++;
      let state = vsctm.INITIAL;
      const src = m[1].split('\n');
      for (const line of src) {
        const r = grammar.tokenizeLine(line, state);
        for (const t of r.tokens) {
          const s = t.scopes[t.scopes.length - 1];
          scopes.set(s, (scopes.get(s) || 0) + 1);
        }
        state = r.ruleStack;
        lines++;
      }
      // a block must end back at the top level (not inside a string/comment)
      if (state.depth > 1) {
        const at = text.slice(0, m.index).split('\n').length;
        bad.push(`${f}:${at}`);
      }
    }
  }
  // and every rule-corpus and lit program
  const progDirs = ['tests/conformance/rules/cases', 'tests/lit'];
  let files = 0;
  const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? walk(path.join(d, e.name)) : (e.name.endsWith('.bzl') ? [path.join(d, e.name)] : []));
  for (const d of progDirs) {
    for (const p of walk(path.join(root, d))) {
      files++;
      let state = vsctm.INITIAL;
      for (const line of fs.readFileSync(p, 'utf8').split('\n')) {
        state = grammar.tokenizeLine(line, state).ruleStack;
        lines++;
      }
      if (state.depth > 1) bad.push(path.relative(root, p));
    }
  }
  console.log(`tokenized ${blocks} bezel blocks and ${files} .bzl files, ${lines} lines`);
  const top = [...scopes.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12);
  for (const [s, n] of top) console.log(`  ${String(n).padStart(7)}  ${s}`);
  if (bad.length) {
    console.log(`${bad.length} block(s) end inside a string or comment:`);
    for (const b of bad.slice(0, 30)) console.log('  ' + b);
    process.exit(1);
  }
})();
