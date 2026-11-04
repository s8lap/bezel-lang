// Strips the «keyword» / ‹identifier› / ⟪code⟫ marks from a contextual fixture
// (test/contextual-fixtures.js) and returns the plain code with each mark's offsets and kind
// ('keyword', 'plain', or 'code': must not be inside a string or comment).
function parse(fixture) {
  let code = '';
  const marks = [];
  const re = /«([^»]*)»|‹([^›]*)›|⟪([^⟫]*)⟫/g;
  let last = 0, m;
  while ((m = re.exec(fixture))) {
    code += fixture.slice(last, m.index);
    const word = m[1] !== undefined ? m[1] : m[2] !== undefined ? m[2] : m[3];
    const kind = m[1] !== undefined ? 'keyword' : m[2] !== undefined ? 'plain' : 'code';
    marks.push({ word, start: code.length, end: code.length + word.length, kind, keyword: kind === 'keyword' });
    code += word;
    last = re.lastIndex;
  }
  code += fixture.slice(last);
  return { code, marks };
}
module.exports = { parse };
