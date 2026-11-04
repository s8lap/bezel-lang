#!/usr/bin/env python3
"""gen_editor_grammars.py — generate Bezel's editor grammars from the spec.

The keyword sets are read from the language spec's Appendix B (B.1: the
unconditionally reserved words and the contextual keywords); nothing is
hand-listed except the *category* each keyword is coloured as and, for a
contextual keyword, the positions (Appendix B.2 / Appendix A) it is coloured
in. Two artifacts are written:

  editors/vscode/syntaxes/bezel.tmLanguage.json   TextMate grammar (VS Code,
                                                   Open VSX, Sublime, Shiki,
                                                   JetBrains TextMate bundles)
  docs/book/theme/bezel-highlight.js              highlight.js language used
                                                   by the mdBook docs site

Usage:
  scripts/gen_editor_grammars.py           regenerate both files
  scripts/gen_editor_grammars.py --check   exit 1 if either file is stale or
                                           Appendix B names a keyword the
                                           category or contextual tables
                                           do not know
                                           (run by scripts/check_docs.sh)
"""
import json
import os
import re
import subprocess
import sys

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True,
                      text=True).stdout.strip() or os.getcwd()
SPEC = os.path.join(ROOT, 'docs', 'Bezel_Language_Spec.md')
TM_OUT = os.path.join(ROOT, 'editors', 'vscode', 'syntaxes', 'bezel.tmLanguage.json')
HLJS_OUT = os.path.join(ROOT, 'docs', 'book', 'theme', 'bezel-highlight.js')

# Colour categories. Every unconditionally reserved word of Appendix B.1 must
# appear in exactly one set; --check fails on a new or a retired keyword so the
# table is kept in step with the spec.
TYPES = {'bool', 'byte', 'char', 'decimal', 'double', 'float', 'int', 'long', 'sbyte',
         'short', 'string', 'uint', 'ulong', 'ushort'}
CONSTANTS = {'true', 'false', 'null'}
VARIABLES = {'this', 'base'}
MODIFIERS = {'abstract', 'async', 'closed', 'const', 'extern', 'internal', 'out', 'override',
             'owned', 'private', 'protected', 'public', 'readonly', 'ref', 'sealed', 'static',
             'virtual'}
DECLARATIONS = {'choice', 'class', 'constructor', 'destructor', 'enum', 'extension', 'import',
                'interface', 'lambda', 'method', 'module', 'property', 'record', 'type', 'var'}
WORD_OPERATORS = {'and', 'as', 'in', 'is', 'mod', 'not', 'or'}
CONTROL = {'await', 'break', 'case', 'catch', 'continue', 'do', 'elif', 'else', 'end',
           'finally', 'for', 'if', 'move', 'return', 'then', 'throw', 'try', 'using', 'when',
           'while', 'with', 'yield'}
OTHER = {'default', 'ensures', 'invariant', 'nameof', 'new', 'requires', 'typeof', 'where'}
CATEGORIES = [TYPES, CONSTANTS, VARIABLES, MODIFIERS, DECLARATIONS, WORD_OPERATORS, CONTROL,
              OTHER]

# Contextual keywords (B.2) are identifiers outside their positions, so the
# grammars colour them only where the position is recognisable lexically, and
# prefer leaving a keyword plain to colouring an identifier. Every B.2 keyword
# is covered except one whose position depends on an enclosing declaration
# rather than on the tokens around it: `value` as the implicit setter parameter
# (an ordinary parameter name there, B.2's note), which stays plain; `value` is
# coloured only as a constraint. The contexts, all from B.2 and the Appendix A
# production named:
#   value notnull sendable hashable  a `where T:` constraint list (Constraint)
#   pure      the member-modifier slot and `pure get` (MethodDecl, Accessor, ...)
#   scoped    a field/parameter modifier and the call-site marker `f(scoped x)`
#   get set   accessor heads and the `private set` auto-property shorthand
#   old       `old(` inside an `ensures (...)` clause
#   result    anywhere inside an `ensures (...)` clause (its keyword reading always
#             wins there, B.2); not after a `.`
#   of        closing a `case` head, on the same or a later line (CaseStmt/CaseExpr)
#   step      after a range `a..b` / `a..<b` (Range)
#   convention, env, variadic   inside an `extern "lib"` block (ExternBlock)
#   symbol    the `:= symbol "..."` alias clause (ExternDecl)
#   serialized (+ skip, optional), equality skip   the member clauses
#   polymorphic   the `polymorphic ("tag": Type, ...)` class-body member
#   operator  after `static method`, after `static` (IfaceStaticOperator) and
#             in `end operator +;` (OperatorBody), each followed by an OpToken
CONTEXT_CONSTRAINTS = {'value', 'notnull', 'sendable', 'hashable'}
CONTEXT_POSITIONAL = {'pure', 'scoped', 'get', 'set', 'old', 'result', 'of', 'step', 'convention',
                      'env', 'variadic', 'symbol', 'serialized', 'equality', 'polymorphic',
                      'operator'}
VISIBILITY = ['public', 'private', 'protected', 'internal']
OP_TOKEN = r'<<|>>|<=|>=|<>|\bmod\b|[-+*/&|^<>=~]'   # A.4 OpToken
STR = r'"(?:[^"\\\n]|\\.)*"'                       # a plain string literal
ID = r'[A-Za-z_][A-Za-z0-9_]*'


def read_keywords():
    text = open(SPEC, encoding='utf-8').read()
    i = text.index('### B.1 Reserved keywords')
    j = text.index('### B.2 Contextual keywords', i)
    b1 = text[i:j]
    m = re.search(r'\*\*Unconditionally reserved:\*\*\s*\n\s*\n(.*?)\n', b1, re.S)
    if not m:
        sys.exit('gen_editor_grammars: Appendix B.1 has no "Unconditionally reserved" list')
    reserved = set(re.findall(r'`([a-z]+)`', m.group(1).split('. (')[0]))
    m = re.search(r'\*\*Contextual \(see', b1)
    ctx_line = b1[m.end():].split('\n', 2)[2] if m else ''
    contextual = set(re.findall(r'`([a-z]+)`', ctx_line.split('\n')[0]))
    return reserved, contextual


def words(s):
    return '|'.join(sorted(s, key=lambda w: (-len(w), w)))



def tm_contextual(reserved, contextual):
    """The TextMate rules for the B.2 positions (see CONTEXT_POSITIONAL)."""
    ctx = 'keyword.other.contextual.bezel'
    mod = 'storage.modifier.contextual.bezel'
    vis = '|'.join(VISIBILITY)
    elem = r'(?:new\s*\(\s*\)|' + ID + r'(?:\.' + ID + r')*(?:<[^;:()<>]*(?:<[^;:()<>]*>[^;:()<>]*)*>)?\??)'
    return [
        # where T: value, notnull, IFoo<T>  (A.5 GenericConstraints / Constraint)
        {'match': r'\b(where)\s+(' + ID + r')\s*(:)(\s*' + elem + r'(?:\s*,\s*' + elem + r')*)',
         'captures': {'1': {'name': 'keyword.other.bezel'}, '2': {'name': 'entity.name.type.parameter.bezel'},
                      '4': {'patterns': [
                          {'match': r'<.*>', 'captures': {'0': {'patterns': [{'include': '$self'}]}}},
                          {'match': r'(?<!\.)\b(?:' + words(CONTEXT_CONSTRAINTS & contextual) + r')\b(?![\w.])',
                           'name': ctx},
                          {'include': '$self'}]}}},
        # `pure` in the member-modifier slot (A.4 MethodDecl, PropertyDecl, IndexerDecl,
        # ConstructorDecl, Iface*); `scoped` on a field or parameter, and as the call-site marker
        {'match': r'\b(pure)(?=\s+(?:method|get|property|constructor|readonly|abstract|virtual|override'
                  r'|sealed|static|owned|' + vis + r')\b)', 'name': mod},
        {'match': r'\b(scoped)(?=\s+(?:readonly\b|' + ID + r'\s*[:,)]))', 'name': mod},
        # accessor heads at the start of a line (A.4 Accessor), and the auto-property
        # visibility shorthand `private set` (AutoAccessorVis)
        {'match': r'^\s*((?:(?:' + vis + r')\s+)?)(?:(pure)\s+)?(get|set)\b(?=\s*(?:;|=>|\(\s*owned\s*\)|$))',
         'captures': {'1': {'patterns': [{'include': '#keywords'}]}, '2': {'name': mod}, '3': {'name': ctx}}},
        {'match': r'\b(' + vis + r')\s+(get|set)\b(?=\s*(?:;|:=|=>|\(\s*owned\s*\)))',
         'captures': {'1': {'name': 'storage.modifier.bezel'}, '2': {'name': ctx}}},
        # `old(` and `result` only inside an `ensures (...)` clause (A.4 ContractClause, §22.16)
        {'begin': r'\b(ensures)\s*(\()', 'end': r'\)',
         'beginCaptures': {'1': {'name': 'keyword.other.bezel'}},
         'patterns': [{'include': '#ensures-body'}]},
        # `of` closes a `case` head (A.6 CaseStmt, A.7 CaseExpr), on the same line or a later
        # one. The head is a begin/end scope so strings and comments in it nest; it gives up at
        # `;`, `=>` or `end`, so a head missing its `of` colours nothing after it.
        {'begin': r'\b(case)\b', 'end': r'\b(of)\b|(?=;|=>|\bend\b)',
         'beginCaptures': {'1': {'name': 'keyword.control.bezel'}},
         'endCaptures': {'1': {'name': 'keyword.control.contextual.bezel'}},
         'patterns': [{'include': '$self'}]},
        # `step` after a range (A.7 Range: Shift (".." | "..<") Shift ("step" Shift)?)
        {'match': r'(\.\.<?)(\s*(?!step\b)[^\s;,"{}](?:(?!\bstep\b|[;,"{}]|=>|:=|\b(?:do|then|of)\b).)*?)\s+(step)\b(?=\s*[-+(\w])',
         'captures': {'1': {'name': 'keyword.operator.bezel'},
                      '2': {'patterns': [{'include': '$self'}]},
                      '3': {'name': ctx}}},
        # extern "lib" convention "cc" … end;  (A.3 ExternBlock); `env` and `variadic` inside it
        {'begin': r'\b(extern)\s+(' + STR + r')(?:\s+(convention)\s+(' + STR + r'))?',
         'end': r'\b(end)\b(?:\s+(extern)\b)?(?=\s*;)',
         'beginCaptures': {'1': {'name': 'storage.modifier.bezel'}, '2': {'name': 'string.quoted.double.bezel'},
                           '3': {'name': ctx}, '4': {'name': 'string.quoted.double.bezel'}},
         'endCaptures': {'1': {'name': 'keyword.control.bezel'}, '2': {'name': 'storage.modifier.bezel'}},
         'patterns': [{'include': '#extern-body'}]},
        # := symbol "name" variadic(N)  (A.3 ExternDecl, VariadicClause)
        {'match': r'(:=)\s*(symbol)\s+(' + STR + r')(?:\s*(variadic)(?=\s*\())?',
         'captures': {'1': {'name': 'keyword.operator.assignment.bezel'}, '2': {'name': ctx},
                      '3': {'name': 'string.quoted.double.bezel'}, '4': {'name': ctx}}},
        # serialized skip | serialized "name" optional? | serialized optional  (A.3 SerializedClause)
        {'match': r'\b(serialized)\s+(?:(skip)\b|(' + STR + r')(?:\s+(optional)\b)?|(optional)\b)',
         'captures': {'1': {'name': ctx}, '2': {'name': ctx}, '3': {'name': 'string.quoted.double.bezel'},
                      '4': {'name': ctx}, '5': {'name': ctx}}},
        # equality skip  (A.3 EqualityClause)
        {'match': r'\b(equality)\s+(skip)\b', 'captures': {'1': {'name': ctx}, '2': {'name': ctx}}},
        # polymorphic ("tag": Type, …);  (A.3 PolymorphicMember: the first PolyCase is `"…" :`)
        {'match': r'\b(polymorphic)\s*(?=\(\s*' + STR + r'\s*:)', 'captures': {'1': {'name': ctx}}},
        # static operator +  (A.4 IfaceStaticOperator) and end operator +;  (OperatorBody);
        # `static method operator +` is in #declarations
        {'match': r'\b(static)\s+(operator)\s*(' + OP_TOKEN + r')',
         'captures': {'1': {'name': 'storage.modifier.bezel'}, '2': {'name': ctx},
                      '3': {'name': 'entity.name.function.operator.bezel'}}},
        {'match': r'\b(end)\s+(operator)\s*(' + OP_TOKEN + r')\s*(?=;)',
         'captures': {'1': {'name': 'keyword.control.bezel'}, '2': {'name': ctx},
                      '3': {'name': 'entity.name.function.operator.bezel'}}},
    ]

def tm_grammar(reserved, contextual):
    def kw(names, scope):
        return {'match': r'\b(?:' + words(names) + r')\b', 'name': scope}
    present = lambda s: s & reserved
    control = reserved - set().union(*(c for c in CATEGORIES if c is not CONTROL))
    escape = {'match': r'\\(?:[\\\'"0abfnrtv{}]|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8}|x[0-9A-Fa-f]{1,4})',
              'name': 'constant.character.escape.bezel'}
    interp = {'begin': r'\{(?!\{)', 'end': r'\}',
              'beginCaptures': {'0': {'name': 'punctuation.section.interpolation.begin.bezel'}},
              'endCaptures': {'0': {'name': 'punctuation.section.interpolation.end.bezel'}},
              'contentName': 'meta.interpolation.bezel',
              'patterns': [{'include': '$self'}]}
    g = {
        '$schema': 'https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json',
        'name': 'Bezel',
        'scopeName': 'source.bezel',
        'fileTypes': ['bzl'],
        'comment': 'GENERATED by scripts/gen_editor_grammars.py from the language spec, '
                   'Appendix A and B. Do not edit by hand.',
        'patterns': [{'include': '#comments'}, {'include': '#strings'},
                     {'include': '#numbers'}, {'include': '#declarations'},
                     {'include': '#end-label'}, {'include': '#contextual'},
                     {'include': '#keywords'}, {'include': '#types'},
                     {'include': '#operators'}],
        'repository': {
            'comments': {'patterns': [
                {'begin': r'///', 'end': r'$', 'name': 'comment.line.documentation.bezel'},
                {'begin': r'//', 'end': r'$', 'name': 'comment.line.double-slash.bezel'},
                {'begin': r'/\*', 'end': r'\*/', 'name': 'comment.block.bezel'},
            ]},
            'strings': {'patterns': [
                {'begin': r'\$@"', 'end': r'"(?!")', 'name': 'string.quoted.double.interpolated.verbatim.bezel',
                 'patterns': [{'match': '""', 'name': 'constant.character.escape.bezel'},
                              {'match': r'\{\{|\}\}', 'name': 'constant.character.escape.bezel'}, interp]},
                {'begin': r'\$"', 'end': r'"', 'name': 'string.quoted.double.interpolated.bezel',
                 'patterns': [escape, {'match': r'\{\{|\}\}', 'name': 'constant.character.escape.bezel'},
                              interp]},
                {'begin': r'@"', 'end': r'"(?!")', 'name': 'string.quoted.double.verbatim.bezel',
                 'patterns': [{'match': '""', 'name': 'constant.character.escape.bezel'}]},
                {'begin': r'"', 'end': r'"', 'name': 'string.quoted.double.bezel', 'patterns': [escape]},
                {'match': r"'(?:\\.|[^'\\])+'", 'name': 'string.quoted.single.bezel'},
            ]},
            'numbers': {'patterns': [
                {'match': r'\b0x[0-9A-Fa-f](?:_*[0-9A-Fa-f])*(?:[uU][lL]?|[lL][uU]?)?\b',
                 'name': 'constant.numeric.hex.bezel'},
                {'match': r'\b0b[01](?:_*[01])*(?:[uU][lL]?|[lL][uU]?)?\b', 'name': 'constant.numeric.binary.bezel'},
                {'match': r'\b[0-9](?:_*[0-9])*(?:\.[0-9](?:_*[0-9])*)?(?:[eE][+-]?[0-9](?:_*[0-9])*)?[fFmM]?\b',
                 'name': 'constant.numeric.decimal.bezel'},
                {'match': r'\b[0-9](?:_*[0-9])*(?:[uU][lL]?|[lL][uU]?)\b', 'name': 'constant.numeric.decimal.bezel'},
            ]},
            'declarations': {'patterns': [
                {'match': r'\b(module)\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)',
                 'captures': {'1': {'name': 'keyword.other.declaration.bezel'},
                              '2': {'name': 'entity.name.namespace.bezel'}}},
                {'match': r'\b(import)\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)',
                 'captures': {'1': {'name': 'keyword.other.declaration.bezel'},
                              '2': {'name': 'entity.name.namespace.bezel'}}},
                {'match': r'\b(class|interface|record|enum|choice|extension|type)\s+([A-Za-z_][A-Za-z0-9_]*)',
                 'captures': {'1': {'name': 'keyword.other.declaration.bezel'},
                              '2': {'name': 'entity.name.type.bezel'}}},
                {'match': r'\b(method|property)\s+(?:(operator)\s*(' + OP_TOKEN + r')|([A-Za-z_][A-Za-z0-9_]*))',
                 'captures': {'1': {'name': 'keyword.other.declaration.bezel'},
                              '2': {'name': 'keyword.other.contextual.bezel'},
                              '3': {'name': 'entity.name.function.operator.bezel'},
                              '4': {'name': 'entity.name.function.bezel'}}},
            ]},
            'end-label': {'match': r'\b(end)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?=;)',
                          'captures': {'1': {'name': 'keyword.control.bezel'},
                                       '2': {'name': 'entity.name.label.bezel'}}},
            'contextual': {'patterns': tm_contextual(reserved, contextual)},
            'ensures-body': {'patterns': [
                {'match': r'(?<!\.)\b(old)(?=\s*\()', 'name': 'keyword.other.contextual.bezel'},
                # `result` reads as the keyword throughout the clause (B.2); `x.result` is a member
                {'match': r'(?<!\.)\b(result)\b', 'name': 'keyword.other.contextual.bezel'},
                {'begin': r'\(', 'end': r'\)', 'patterns': [{'include': '#ensures-body'}]},
                {'begin': r'\b(case)\b', 'end': r'\b(of)\b|(?=;|=>|\bend\b)',
                 'beginCaptures': {'1': {'name': 'keyword.control.bezel'}},
                 'endCaptures': {'1': {'name': 'keyword.control.contextual.bezel'}},
                 'patterns': [{'include': '#ensures-body'}]},
                {'include': '$self'}]},
            'extern-body': {'patterns': [
                # the user-data marker: a bare `env` list element; `env:` is a parameter named env
                # and `: env` a type, both identifiers (B.2, A.3 ExternParam)
                {'match': r'(?:(?<=[(,<])|^)\s*\b(env)\b(?=\s*[,)])',
                 'captures': {'1': {'name': 'keyword.other.contextual.bezel'}}},
                {'match': r'\b(variadic)\s*(?=\(\s*[0-9])', 'captures': {'1': {'name': 'keyword.other.contextual.bezel'}}},
                {'include': '$self'}]},
            'keywords': {'patterns': [p for p in [
                kw(present(CONSTANTS), 'constant.language.bezel'),
                kw(present(VARIABLES), 'variable.language.bezel'),
                kw(present(TYPES), 'storage.type.primitive.bezel'),
                kw(present(MODIFIERS), 'storage.modifier.bezel'),
                kw(present(DECLARATIONS), 'keyword.other.declaration.bezel'),
                kw(present(WORD_OPERATORS), 'keyword.operator.word.bezel'),
                kw(present(OTHER), 'keyword.other.bezel'),
                kw(control, 'keyword.control.bezel'),
            ] if p['match'] != r'\b(?:)\b']},
            'types': {'match': r'\b[A-Z][A-Za-z0-9_]*\b', 'name': 'entity.name.type.bezel'},
            'operators': {'patterns': [
                {'match': r':=|\+=|-=|\*=|/=|\?\?=', 'name': 'keyword.operator.assignment.bezel'},
                {'match': r'=>|->', 'name': 'keyword.operator.arrow.bezel'},
                {'match': r'\?\?|\?\.|\?\[|!(?![=])', 'name': 'keyword.operator.null.bezel'},
                {'match': r'<=|>=|<>|=|<|>', 'name': 'keyword.operator.comparison.bezel'},
                {'match': r'\.\.<|\.\.|\+|-|\*|/|%|&|\||\^|~|<<|>>', 'name': 'keyword.operator.bezel'},
            ]},
        },
    }
    return json.dumps(g, indent=2, ensure_ascii=False) + '\n'


def hljs_language(reserved, contextual):
    keyword = sorted((reserved - TYPES - CONSTANTS - VARIABLES))
    # highlight.js 10.1.1 (the copy mdBook 0.4.52 bundles) has no `match`/`beginScope`, so a
    # contextual keyword is coloured either by a className mode whose lookahead checks what
    # follows it, or by a mode that begins at the token leading into the position and ends on
    # the keyword, with the keyword in that mode's own keyword list. No lookbehind is used, so
    # the language compiles in every browser mdBook supports. The positions are the TextMate
    # grammar's (see CONTEXT_POSITIONAL); where_end stops a `where T:` list at a reserved word.
    where_end = words(reserved - {'class', 'new'})
    js = r'''// GENERATED by scripts/gen_editor_grammars.py from the language spec, Appendix A and B.
// Do not edit by hand. Registers the `bezel` language with mdBook's highlight.js
// (10.1.1) and re-highlights the book's ```bezel blocks (book.js highlights before
// this file loads). Contextual keywords (Appendix B.2) are keywords only in the
// positions the modes below recognise; elsewhere they stay identifiers.
(function () {
  if (typeof hljs === 'undefined') { return; }
  hljs.registerLanguage('bezel', function (hljs) {
    var KEYWORDS = {
      keyword: @KEYWORD@,
      built_in: @BUILTIN@,
      literal: @LITERAL@
    };
    var KW = function (extra) {
      return { keyword: KEYWORDS.keyword + ' ' + extra, built_in: KEYWORDS.built_in, literal: KEYWORDS.literal };
    };
    var CTX = function (re) { return { className: 'keyword', begin: re }; };
    var INTERP = { className: 'subst', begin: /\{(?!\{)/, end: /\}/, keywords: KEYWORDS };
    var COMMENTS = [hljs.COMMENT('///', '$'), hljs.C_LINE_COMMENT_MODE, hljs.C_BLOCK_COMMENT_MODE];
    var STRING = { className: 'string', variants: [
      { begin: /\$@"/, end: /"(?!")/, contains: [{ begin: /""/ }, INTERP] },
      { begin: /\$"/, end: /"/, contains: [hljs.BACKSLASH_ESCAPE, INTERP] },
      { begin: /@"/, end: /"(?!")/, contains: [{ begin: /""/ }] },
      { begin: /"/, end: /"/, contains: [hljs.BACKSLASH_ESCAPE] },
      { begin: /'(?:\\.|[^'\\])+'/ }
    ] };
    var NUMBER = { className: 'number', begin: /\b(?:0x[0-9A-Fa-f][0-9A-Fa-f_]*|0b[01][01_]*|[0-9][0-9_]*(?:\.[0-9][0-9_]*)?(?:[eE][+-]?[0-9][0-9_]*)?)(?:[uU][lL]?|[lL][uU]?|[fFmM])?\b/ };
    var TYPE = { className: 'type', begin: /\b[A-Z][A-Za-z0-9_]*\b/ };
    var PLAIN = COMMENTS.concat([STRING, NUMBER, TYPE]);
    var PARENS = { begin: /\(/, end: /\)/, keywords: KEYWORDS };
    PARENS.contains = ['self'].concat(PLAIN);
    var METHOD_TITLE = { className: 'title', begin: /\bmethod\s+/, end: /[A-Za-z_][A-Za-z0-9_]*/, excludeBegin: true };

    // old( inside an `ensures (...)` clause,
    // and `result`, which reads as the keyword throughout the clause; `x.result` is a member
    var MEMBER = { begin: /\.[A-Za-z_][A-Za-z0-9_]*/ };
    var IN_ENSURES = [MEMBER, CTX(/\bold\b(?=\s*\()/), CTX(/\bresult\b/)];
    var ENSURES_PARENS = { begin: /\(/, end: /\)/, keywords: KEYWORDS };
    var ENSURES_CASE = { begin: /\bcase\b/, end: /\bof\b|(?=;|=>|\bend\b)/, keywords: KW('of') };
    ENSURES_PARENS.contains = ['self', ENSURES_CASE].concat(IN_ENSURES, PLAIN);
    ENSURES_CASE.contains = [ENSURES_PARENS].concat(IN_ENSURES, PLAIN);
    var ENSURES = { begin: /\bensures\s*\(/, end: /\)/, keywords: KEYWORDS, contains: [ENSURES_PARENS, ENSURES_CASE].concat(IN_ENSURES, PLAIN) };
    // `of` closing a `case` head, on any line; the head gives up at `;`, `=>` or `end`
    var CASE = { begin: /\bcase\b/, end: /\bof\b|(?=;|=>|\bend\b)/, keywords: KW('of'), contains: [PARENS].concat(PLAIN) };
    // `step` after a range a..b / a..<b
    var STEP = {
      begin: /\.\.<?(?=[ \t]*(?!step\b)[^\s;,{}"](?:(?!\b(?:do|then|of)\b|=>|:=)[^;\n,{}"])*?\sstep\b)/,
      end: /(?=[;\n,{}"]|=>|:=|\b(?:do|then|of)\b)/,
      keywords: KEYWORDS,
      contains: [PARENS, { className: 'keyword', begin: /\bstep\b(?!\s+step\b)(?=\s*[-+(\w])/, endsParent: true }, NUMBER, TYPE]
    };
    // := symbol "name"   (also inside extern blocks)
    var SYMBOL = { begin: /:=\s*(?=symbol\s+")/, end: /\bsymbol\b/, keywords: { keyword: 'symbol' } };
    // extern "lib" convention "cc" ... end;  with the `env` marker and `variadic(N)` inside
    var EXTERN = {
      begin: /\bextern(?=\s+")/, end: /\bend\b(?:\s+extern\b)?/, keywords: KEYWORDS,
      contains: COMMENTS.concat([
        CTX(/\bconvention\b(?=\s*")/), STRING, NUMBER,
        { begin: /:\s*(?=env\b)/, end: /\benv\b/ },   // `x: env` names a type: plain
        CTX(/\benv\b(?=\s*[,)])/),
        CTX(/\bvariadic\b(?=\s*\(\s*[0-9])/),
        SYMBOL, METHOD_TITLE, TYPE])
    };
    // serialized skip | serialized "name" optional? | serialized optional;  equality skip
    var SERIALIZED = {
      begin: /\bserialized\b(?=\s+(?:skip\b|optional\b|"))/, end: /\b(?:skip|optional)\b|(?=\S)/,
      keywords: { keyword: 'serialized skip optional' }, contains: [STRING]
    };
    var EQUALITY = { begin: /\bequality(?=\s+skip\b)/, end: /\bskip\b/, keywords: { keyword: 'equality skip' } };
    var POLYMORPHIC = CTX(/\bpolymorphic\b(?=\s*\(\s*"(?:[^"\\\n]|\\.)*"\s*:)/);
    // static method operator +, static operator + (interfaces), end operator +;
    var OPERATOR = {
      begin: /\b(?:method|static|end)\s+(?=operator\s*(?:@OP@))/, end: /\boperator\b/, keywords: KW('operator')
    };
    var SCOPED = CTX(/\bscoped\b(?=\s+(?:readonly\b|[A-Za-z_][A-Za-z0-9_]*\s*[:,)]))/);
    var PURE = CTX(/\bpure\b(?=\s+(?:method|get|property|constructor|readonly|abstract|virtual|override|sealed|static|owned|public|private|protected|internal)\b)/);
    // accessor heads at the start of a line, and the `private set` auto-property shorthand
    var ACCESSOR_LINE = {
      begin: /^[ \t]*(?:(?:public|private|protected|internal)[ \t]+)?(?:pure[ \t]+)?(?=(?:get|set)\b[ \t]*(?:;|=>|\([ \t]*owned[ \t]*\)|$))/,
      end: /\b(?:get|set)\b/, keywords: KW('get set pure')
    };
    var ACCESSOR_VIS = {
      begin: /\b(?:public|private|protected|internal)\s+(?=(?:get|set)\b\s*(?:;|:=|=>|\(\s*owned\s*\)))/,
      end: /\b(?:get|set)\b/, keywords: KW('get set')
    };
    // where T: value, notnull, IFoo<T>
    var GENERIC_ARGS = { begin: /</, end: />/, contains: ['self', TYPE] };
    var WHERE = {
      begin: /\bwhere(?=\s+[A-Za-z_][A-Za-z0-9_]*\s*:)/,
      end: /(?=[;{\n=]|\b(?:@WHERE_END@)\b)/,
      keywords: KEYWORDS,
      contains: COMMENTS.concat([GENERIC_ARGS, MEMBER, CTX(/\b(?:@CONSTRAINTS@)\b(?![\w.<]|\s*:)/), TYPE])
    };
    return {
      name: 'Bezel',
      aliases: ['bzl'],
      keywords: KEYWORDS,
      contains: COMMENTS.concat([
        STRING,
        NUMBER,
        EXTERN, ENSURES, CASE, STEP, SYMBOL, SERIALIZED, EQUALITY, POLYMORPHIC, OPERATOR,
        SCOPED, PURE, ACCESSOR_LINE, ACCESSOR_VIS, WHERE,
        { className: 'title', begin: /\b(?:class|interface|record|enum|choice|extension)\s+/, end: /[A-Za-z_][A-Za-z0-9_]*/, excludeBegin: true },
        METHOD_TITLE,
        TYPE
      ])
    };
  });
  if (typeof document === 'undefined') { return; }
  document.querySelectorAll('code.language-bezel').forEach(function (block) {
    block.innerHTML = block.textContent.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    if (hljs.highlightElement) { hljs.highlightElement(block); } else { hljs.highlightBlock(block); }
  });
})();
'''
    subst = {
        '@KEYWORD@': json.dumps(' '.join(keyword)),
        '@BUILTIN@': json.dumps(' '.join(sorted(TYPES & reserved))),
        '@LITERAL@': json.dumps(' '.join(sorted((CONSTANTS | VARIABLES) & reserved))),
        '@OP@': OP_TOKEN.replace('/', '\\/'),
        '@WHERE_END@': where_end,
        '@CONSTRAINTS@': words(CONTEXT_CONSTRAINTS & contextual),
    }
    for k, v in subst.items():
        js = js.replace(k, v)
    return js


def main():
    check = '--check' in sys.argv[1:]
    reserved, contextual = read_keywords()
    known = set().union(*CATEGORIES)
    problems = []
    for w in sorted(reserved - known):
        problems.append(f'Appendix B.1 reserves `{w}`, which has no colour category in this script')
    for w in sorted(known - reserved):
        problems.append(f'category table lists `{w}`, which Appendix B.1 no longer reserves')
    ctx_known = CONTEXT_CONSTRAINTS | CONTEXT_POSITIONAL
    for w in sorted(ctx_known - contextual):
        problems.append(f'contextual table lists `{w}`, which Appendix B.1 does not name as contextual')
    for w in sorted(contextual - ctx_known):
        problems.append(f'Appendix B.1 names `{w}` as contextual, which no contextual table in this '
                        'script places')
    outs = {TM_OUT: tm_grammar(reserved, contextual), HLJS_OUT: hljs_language(reserved, contextual)}
    for path, content in outs.items():
        rel = os.path.relpath(path, ROOT)
        if check:
            if not os.path.isdir(os.path.dirname(os.path.dirname(path))):
                # a partial tree (a scratch copy of docs/ and scripts/ only)
                print(f'GRAMMAR: {rel} skipped (not in this tree)')
                continue
            try:
                cur = open(path, encoding='utf-8').read()
            except FileNotFoundError:
                cur = None
            if cur != content:
                problems.append(f'{rel} is stale: run scripts/gen_editor_grammars.py')
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'w', encoding='utf-8').write(content)
            print(f'wrote {rel}')
    if problems:
        print('gen_editor_grammars: ' + str(len(problems)) + ' problem(s):')
        print('\n'.join('  ' + p for p in problems))
        sys.exit(1)
    if check:
        print(f'GRAMMAR: clean ({len(reserved)} reserved and {len(contextual)} contextual keywords; '
              'editor grammars up to date)')


if __name__ == '__main__':
    main()
