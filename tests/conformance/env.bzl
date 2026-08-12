// Ambient bindings for conformance example fragments — the value-level
// counterpart of preamble.bzl.
//
// preamble.bzl supplies illustrative helper TYPES the surrounding prose assumes
// (Connection, Point, BankAccount, …). This file supplies the illustrative
// VALUES and MEMBERS a fragment assumes: `case score of 90..100 => "A"` is a
// complete claim about §19's `case` form, but `score` is a pronoun the prose
// supplies in English and the block does not declare.
//
// Format. An entry is introduced by
//
//     //@ <local|member|type> <name>
//
// and runs to the next header. `local` is spliced at the top of the synthesized
// `Main` body, `member` into the synthesized `class W`, `type` beside the
// module's other top-level declarations. A case opts in by name in its manifest
// `notes` field: `env:score` (several: `env:score,grade`).
//
// THE RULE FOR ADDING AN ENTRY. Its type must be forced by the fragment's own
// text, or stated in the prose immediately around it — and the derivation goes
// in the comment, so a reviewer can check it. Do NOT pick a spelling by trying
// them until the compiler stops complaining. That is circular, and on this
// compiler it does not even measure conformance: an auto-solver run that sized
// this bucket "solved" cases by binding called names to `int` (#3153: a call on
// a non-callable value compiles and is silently dropped) and by binding `?.`
// receivers to `int` (#3155: `?.` never resolves its member). A binding that
// only works because of a hole proves nothing about the document's claim.
//
// If a fragment's assumed name has no forced type, leave the case skipped. A
// row that cannot fail is worse than no row.

//@ local score
// spec §19.1 Forms, §19.7 Termination, §20 The `if` Expression, §21.2 The
// `if` statement (v0.254) — the grade-band examples. Integral and ordered: the fragments
// match it against the range patterns `90..100`, `80..89`, `70..<80` and compare
// it with `score >= 90`.
var score: int := 85;

//@ local grade
// spec §21.2 The `if` statement (v0.254) — the `if`/`elif` form assigns the string arms "A".."F" to it.
var grade: string := "";

//@ local delta
// spec §19.1 Forms (v0.254) — the negative-range-bound example matches it against `-10..-6`,
// `..<0`, and `0` — a signed integral.
var delta: int := -3;

//@ local temperature
// spec §19.4 Guards (v0.254) — the guarded-arm example binds it with `var t` and guards `t > 30` /
// `t < 0`, then interpolates it.
var temperature: int := 21;

//@ local i
// spec §21.3 The `while` statement (v0.254): `while i < 10 do … i := i + 1; end;` — a mutable
// integral counter.
var i: int := 0;

//@ local month
// spec §19.7 Termination (v0.254) — the statement-position `case` matches it against the literal arm `1`.
var month: int := 1;

//@ local amount
// spec §21.2 The `if` statement (v0.254) — the example guards `amount > 0` and adds it to `Balance`.
var amount: decimal := 10m;

//@ local Balance
// Same fragment: `Balance := Balance + amount;` — assigned, and summed with a
// `decimal`. (The prose means a property; an ambient mutable of the same type is
// what the fragment's own statements require of it.)
var Balance: decimal := 0m;

//@ local user
// spec §18.7 Interpolated verbatim strings (v0.254) — the example puts it in a Windows user
// path, `$@"C:\Users\{user}\Documents\report.pdf"` — a name, i.e. a string.
var user: string := "alice";

//@ local maybe
// Guide 2 §1 Non-nullable by default (v0.250): `maybe ?? "Anonymous"` needs a nullable left
// operand, `maybe!` asserts non-null, `if maybe <> null then … maybe.Length`
// narrows it and reads a `string` member.
var maybe: string? := "text";

//@ local numbers
// spec §27.4 Parameter type inference and §27.6's method-reference example
// (v0.254): `numbers.Select(lambda(x) => x * 2)`,
// `.Where(lambda(x) => x > 0)`, `.OrderBy(Math.Abs)` — a sequence of a signed
// arithmetic element type.
var numbers: Array<int> := [3, -1, 2];

//@ local source
// spec §29.2 Desugaring of `for` (v0.254) — the examples enumerate it (`for var x in source`,
// and the spelled-out `GetEnumerator`/`MoveNext`/`Current` expansion).
var source: Array<int> := [1, 2, 3];

//@ member _name
// spec §13.6 Accessor visibility (v0.254) — the example is `public property Name: string
// get => _name; private set => _name := value;` — the backing field's type is
// fixed by the property's declared type.
  private _name: string := "";

//@ member _balance
// spec §23.3 Block-form properties (v0.254) — the validating-setter example is `public property Balance: decimal
// get => _balance; set => … _balance := value;` — likewise fixed by the
// property's declared type, and the setter compares against the `decimal`
// literal `0m`.
  private _balance: decimal := 0m;
