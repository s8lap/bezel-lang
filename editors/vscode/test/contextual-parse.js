// Strips the «keyword» / ‹identifier› marks from a contextual fixture (test/contextual-fixtures.js)
// and returns the plain code with each mark's offsets.
function parse(fixture) {
  let code = '';
  const marks = [];
  const re = /«([^»]*)»|‹([^›]*)›/g;
  let last = 0, m;
  while ((m = re.exec(fixture))) {
    code += fixture.slice(last, m.index);
    const word = m[1] !== undefined ? m[1] : m[2];
    marks.push({ word, start: code.length, end: code.length + word.length, keyword: m[1] !== undefined });
    code += word;
    last = re.lastIndex;
  }
  code += fixture.slice(last);
  return { code, marks };
}
module.exports = { parse };
