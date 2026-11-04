// Checks that the generated TextMate grammar colours Bezel's contextual keywords (spec Appendix
// B.2) only in their positions: every «word» in test/contextual-fixtures.js must get a keyword or
// storage scope and every ‹word› must not. Usage (from editors/vscode):
//   npm install --no-save vscode-textmate@9 vscode-oniguruma@2
//   node test/contextual.js
const fs = require('fs');
const path = require('path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');
const { parse } = require('./contextual-parse');

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
const isKeyword = (scope) => /^(keyword|storage)\./.test(scope) && !scope.startsWith('keyword.operator');

(async () => {
  const grammar = await registry.loadGrammar('source.bezel');
  let checked = 0;
  const bad = [];
  for (const fixture of require('./contextual-fixtures')) {
    const { code, marks } = parse(fixture);
    // innermost scope of every character offset
    const scopeAt = [];
    let state = vsctm.INITIAL;
    let base = 0;
    for (const line of code.split('\n')) {
      const r = grammar.tokenizeLine(line, state);
      for (const t of r.tokens) {
        for (let i = t.startIndex; i < t.endIndex; i++) scopeAt[base + i] = t.scopes[t.scopes.length - 1];
      }
      state = r.ruleStack;
      base += line.length + 1;
    }
    for (const m of marks) {
      checked++;
      const scope = scopeAt[m.start] || 'source.bezel';
      const same = scopeAt.slice(m.start, m.end).every((s) => s === scopeAt[m.start]);
      if (m.keyword !== isKeyword(scope) || !same) {
        bad.push(`${JSON.stringify(fixture)}: \`${m.word}\` at ${m.start} has ${scope}, expected ` +
                 (m.keyword ? 'a keyword scope' : 'no keyword scope'));
      }
    }
  }
  console.log(`checked ${checked} contextual positions in the TextMate grammar`);
  if (bad.length) {
    console.log(`${bad.length} wrong:`);
    for (const b of bad) console.log('  ' + b);
    process.exit(1);
  }
})();
