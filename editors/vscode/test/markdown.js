// Checks the Markdown injection grammar: tokenizes Markdown files with VS Code's
// Markdown grammar plus the injection, and verifies that every line inside a
// ```bezel fence is tokenized as Bezel (meta.embedded.block.bezel / source.bezel)
// and that no line outside such a fence is. Usage (from editors/vscode):
//   npm install --no-save vscode-textmate@9 vscode-oniguruma@2
//   MARKDOWN_GRAMMAR=path/to/markdown.tmLanguage node test/markdown.js [files...]
// (markdown.tmLanguage: microsoft/vscode-markdown-tm-grammar, syntaxes/markdown.tmLanguage)
const fs = require('fs');
const path = require('path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

const here = path.join(__dirname, '..');
const root = path.resolve(here, '..', '..');
const mdPath = process.env.MARKDOWN_GRAMMAR;
if (!mdPath) { console.error('set MARKDOWN_GRAMMAR'); process.exit(2); }
const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
const onigLib = oniguruma.loadWASM(wasm).then(() => ({
  createOnigScanner: (p) => new oniguruma.OnigScanner(p),
  createOnigString: (s) => new oniguruma.OnigString(s),
}));
const files = {
  'text.html.markdown': mdPath,
  'source.bezel': path.join(here, 'syntaxes', 'bezel.tmLanguage.json'),
  'markdown.bezel.codeblock': path.join(here, 'syntaxes', 'bezel.markdown.tmLanguage.json'),
};
const registry = new vsctm.Registry({
  onigLib,
  loadGrammar: async (scope) => files[scope]
    ? vsctm.parseRawGrammar(fs.readFileSync(files[scope], 'utf8'), files[scope]) : null,
  getInjections: (scope) => scope === 'text.html.markdown' ? ['markdown.bezel.codeblock'] : [],
});

(async () => {
  const md = await registry.loadGrammar('text.html.markdown');
  const targets = process.argv.slice(2).length ? process.argv.slice(2)
    : fs.readdirSync(path.join(root, 'docs')).filter((f) => f.endsWith('.md')).map((f) => path.join(root, 'docs', f));
  let bad = 0, inside = 0, blocks = 0;
  for (const file of targets) {
    let state = vsctm.INITIAL, fence = null;
    const lines = fs.readFileSync(file, 'utf8').split('\n');
    lines.forEach((line, i) => {
      const r = md.tokenizeLine(line, state);
      state = r.ruleStack;
      const m = line.match(/^(\s*)(`{3,}|~{3,})\s*(\S*)/);
      if (fence === null && m) { fence = m[3]; if (fence === 'bezel') blocks++; return; }
      if (fence !== null && m && !m[3]) { fence = null; return; }
      if (!line.trim()) return;
      const embedded = r.tokens.some((t) => t.scopes.includes('meta.embedded.block.bezel'));
      const wantBezel = fence === 'bezel';
      if (wantBezel) inside++;
      if (embedded !== wantBezel) {
        if (bad < 20) console.log(`${path.relative(root, file)}:${i + 1}: ${wantBezel ? 'not highlighted' : 'highlighted outside a bezel fence'}`);
        bad++;
      }
    });
  }
  console.log(`${targets.length} files, ${blocks} bezel blocks, ${inside} non-blank lines inside them; ${bad} mismatches`);
  process.exit(bad ? 1 : 0);
})();
