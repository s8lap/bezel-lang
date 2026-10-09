# Proposal 0164 — Every naming of a generic member is an instantiation site: method-reference formations, receiver-position type names and extension-block members

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": option C (SIMPLIFY C9 and C11, #5784 reading A) — a method reference over a generic method names its type arguments and its formation is a §14.1 instantiation site; a generic type named in receiver position is a site and a §14.6 edge; each member of a generic extension block is read as a generic method declaring the block's parameters, bound by the receiver or qualifier and never by an explicit list; critique round 1 applied, shared with proposal 0165, which added the overloaded-group rule, kept the explicit list counting the member's own parameters, fixed an Appendix A.7 comment, and gave #5764's assembly check the edges of internal implementations; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5777](https://github.com/s8lap/bezel-lang/issues/5777) (`spec-defect`: a method reference over a generic method binds type parameters by no stated rule and is no listed instantiation site), [#5784](https://github.com/s8lap/bezel-lang/issues/5784) (`spec-question`: is a generic type named in receiver position of a static member access an instantiation site?), [#5761](https://github.com/s8lap/bezel-lang/issues/5761) (`spec-defect`: a generic extension block's parameter has no stated check site); one `spec-tracking` compiler tracker is filed at landing (§8) |
| **Target module(s)** | **spec**: §3.6 rule 2 (the method clause), §14.1 *Instantiation and its site* (the site list), §14.2 *Dispatched generic methods* (the exclusion names formations), §14.5 phase 2 (the method-reference clause), §14.6 (the request forms), §22.12 (the generics-vs-less-than rule), §26.4 (the closing paragraph), §26.5.1 (the `First` annotation), §27.6 (one new paragraph, the extension-reference sentence), §28.14 (one example), Appendix A.7 (two grammar comments), Appendix C.1 and C.4 (one bullet each). **rationale**: §10 (one dated paragraph). **code-generation reference**: §3.3 (one row), §7.4 (one *Looks right, is rejected* pair). **`DECLINED.md`**: four `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The generics matrix's first run (`docs/research/generics-matrix/runs/v0.333.json`), root causes RC39, RC49 and RC21 of `scratch/matrix2/ROOTS.md`, and the simplification review's candidates C9 and C11 (`scratch/matrix2/SIMPLIFY.md`). The owner directed on 2026-10-08: "Please resolve and decide all known spec related issues. Do proposals for issues that need them", and on 2026-10-09: "Please also resolve all findings autonomously". It needs a proposal because C9 removes an admission the text presupposes — a reference to a generic method with no type-argument list (§28.14's `b.Put` over `Put<T>(ref x: T)`). |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Three places name a generic declaration at type arguments without being on §14.1's list of **instantiation sites**, so no rule the list drives — §3.6 rule 2's per-instantiation requirements, §14.2's constraint check and its dispatched-method exclusion, §28.3's two bare-parameter exclusions, §14.6's request graph — is read there:

1. a **method reference formed over a generic method** (`var k: (Array<Object>, Pair) -> () := Util.Keep;` over `Keep<T>(sink: Array<Object>, x: T)`), whose own type parameters no rule binds at all;
2. a **generic type named in receiver position of a static member access** (`Reg<Pair>.Put(new Pair(it))`, the form §22.12 prescribes for every static access);
3. a **generic extension block's own parameter** (`extension IEnumerable<T>`), whose member bodies' uses of `T` §3.6 rule 2 attributes to neither a generic type nor a generic method.

**Decision.** One mechanism, three clauses, no inference rule and no new keyword:

- **A reference to a generic method names its type arguments** (`Util.Keep<Pair>`, `s.Put<[2 owned]Item>`), and a reference to a generic method with no list is a compile error naming the arguments to supply (SIMPLIFY C9). The §22.12 generics-vs-less-than rule admits the closing `>` before a token that cannot begin an operand (`)`, `]`, `,`, `;`), which is what lets `xs.Select(Util.Id<int>)` parse.
- **§14.1's site list gains three entries**: a method-reference formation over a generic member, or over a member of a generic extension block, at the type arguments its list or its qualifier supplies; a type name in receiver position of a static member access or at the head of a member chain (#5784's reading A); and a call of a generic extension block's member at the block arguments the receiver binds. §14.6's request forms gain the same three.
- **A generic extension block's parameters are read as each member's** (SIMPLIFY C11): each member of the block is read, for §3.6 rule 2, §14.1 and §14.2, as a generic method declaring the block's parameters before its own, bound by the receiver (at a call) or the qualifier (at a formation) and never by an explicit type-argument list (which keeps supplying the member's own parameters only, `xs.OfType<int>()`), so §3.6 rule 2's method clause, §14.1's call and formation sites and §14.2's dispatched rule apply verbatim. The sentence "No requirement of §3.6 rule 2 is carried from the head" goes.

Measured reach: **zero** method references to generic methods in `tests/`, `docs/` and the two conformance corpora (the §28.14 example and its rule case `s28_14_0025` gain a type-argument list, verdict unchanged); every static-receiver access and every generic extension call keeps compiling (reading A is what §26.5.1's own `First` annotation and `DECLINED.md`'s #5629 row already presuppose). Newly admitted: nothing.

---

## 1. The problem

### 1.1 Method references over generic methods (#5777, RC39)

§28.14's method-reference formation bar presupposes the form: it rejects a reference "at a bare type parameter that the formation's substitution makes a function type (… `var put: (ref () -> int) -> () := b.Put;` over `Put<T>(ref x: T)`)". But nothing says what "the formation's substitution" is. §27.6 asks only that "its parameter and return types unify with the target's under the standard implicit-conversion rules"; §14.5 phase 2 binds "type parameters free in that target … by the winning candidate's signature", which for `xs.Select(Util.Id)` still contains `Id<T>`'s own `T`; Appendix A.7's `GenericArgs` comment speaks of explicit arguments "at a call site"; and every check keyed on a substitution names a *call*: §14.1 ("a call to a generic member at explicit or inferred type arguments"), §3.6 rule 2 ("a call … among them"), §14.5 *Constraints* ("after inference completes"), §14.2's dispatched exclusion ("a call of one"), both §28.3 exclusions ("at the instantiation").

```bezel
class Item
end Item;
record Pair(It: Item);
interface ISink
  method Put<T>(x: T);       // dispatched: bodies assume T copyable (§14.2)
end ISink;
class Dup: ISink
  public method Put<T>(x: T)  var a := x; var b := x;  end Put;
end Dup;
static class Util
  public static method Keep<T>(sink: Array<Object>, x: T)
    sink.Add(x);             // bare T to Object: a §9.7 requirement per instantiation
  end Keep;
end Util;
class Run
  private owned _sink: Array<Object> := new();
  public method Go(s: ISink)
    using var it := new Item();
    var k: (Array<Object>, Pair) -> () := Util.Keep;  // T := Pair by no stated rule
    k(_sink, new Pair(it));  // the invocation reads only the function type
    var put: ([2 owned]Item) -> () := s.Put;          // the exclusion names calls only
    var arr: [2 owned]Item := [new Item(), new Item()];
    put(arr);                // at a Dup, both copies own the same two Items
  end Go;                    // `it` disposed; _sink keeps a boxed Pair borrowing it
end Run;
```

The same holds for a declared constraint (`Uniq<T> where T: hashable` referenced at a non-hashable record, cell R3/C4·mref), for the `Task`-family and nested-slot exclusions at a formation (`Gen.Apply<Task<int>>`, R8/C2·mref) and for a reference to a member of a generic extension block (`IReadOnlyList<int?>.AsObjects` viewed at `IReadOnlyList<Object?>`, R7/C10·mref — memory-unsafe under §12.9's reference-type condition).

### 1.2 A generic type named in receiver position (#5784, RC49)

§14.1 defines the site by enumeration: "a constructed type in a field, local, parameter, or return type annotation, in a base clause or interface list, in a `where` bound, in a choice payload or inline-array element type, in a type alias, or as a type argument of another constructed type is an instantiation site, as are a `new` expression, a `typeof` or `default` operand, and a call to a generic member at explicit or inferred type arguments." The receiver position of §22.12 ("A static member of a *generic* class is accessed by supplying the type arguments on the type name in receiver position") is not listed, and a static method of a generic type is not itself a generic member. Under the enumeration reading a type reached only through its statics has no site: its §9.7 bullets (`Reg<Pair>.Put(new Pair(it))` storing a boxed `Pair` into a static `Array<T>`), §7.10's copy demand (`Twin<[1 owned]Conn?>.Twice(x)`), §9.2's equality use (`Reg<R>.Same(x, y)`) are never read, and a static initializer's `A<Array<T>>.N` is no §14.6 edge.

### 1.3 A generic extension block's own parameter (#5761, RC21)

§3.6 rule 2 reads a requirement at two kinds of site only: "A use of a generic **type's** parameter … is therefore a requirement of the constructed type, checked at every site that names it …; a use of a generic **method's** own parameter is checked at each instantiation of the method, a call at explicit or inferred type arguments among them." A block-level parameter is neither, and §26.4 says only "No requirement of §3.6 rule 2 is carried from the head: each receiver the block applies to is an instantiation formed, and checked, where its type is named" — true of the *head*, read by the matrix as true of the *body*. Every requirement kind is affected (the ten cells R1–R10/C10): `extension IReadOnlyList<T>`'s `AsObjects(): IReadOnlyList<Object?> => this;` at `T = int?` reads `int?` storage as references under the no-site reading; `extension IEnumerable<T>`'s `KeepAll(sink: Array<Object>)` boxes a `Pair` into an escaping collection at `T = Pair`.

### 1.4 What the compiler does today

Source-read at `429fd72`; no build was run.

- `src/parser/Parser_Exprs.cpp` (`tokensLookLikeMethodTypeArgs`, the §22.12 scan) admits a method type-argument list only when the closing `>` is followed by `(`; `tokensLookLikeReceiverTypeArgs` only before `.`. So `Util.Keep<Pair>` as a value is a parse error today: the explicit-list spelling this proposal requires is spec-ahead-of-impl.
- `src/sema/ExprTypecheck.cpp:4692` (`buildMethodRefType`) forms a reference's function type from the declared parameter types with the referenced method's own type parameters left unsubstituted; no binding against the target exists. The §28.14 bar's `b.Put` example is pinned by rule case `s28_14_0025` (no `baseline.tsv` row).
- `src/sema/TypeResolution_Names.cpp:2648` (`recordStaticReceiverInstantiation`, #3160) records `G<Arg>.M(…)` as an instantiation for every Pass-2 check that reads `instantiations_`: the compiler already takes reading A for the receiver position.
- `src/sema/TypeResolution_Names.cpp:544` (`bindExtensionTargetTypeParams`) binds a block's parameters; whether a block member's interior requirements are read at each extension call is not settled by reading (HYPOTHESIS: they are not, as no per-instantiation requirement reading exists for method-level parameters either — the general §3.6 rule 2 catch-up).

---

## 2. Options

### 2.1 For the method reference (RC39)

| | Option | `xs.Select(Util.Id)` | `var k: (Array<Object>, Pair) -> () := Util.Keep;` | What is written |
|---|---|---|---|---|
| **A** | Status quo: the form is admitted, its binding and site unstated | compiles, `T` unbound | compiles; the `Pair` escapes | nothing |
| **B** | Bind the referenced method's own parameters by unifying its declared signature against the target function type, exactly and slot-wise (#5777 recommendation (a)); the formation is a site | binds `T := int` against the substituted target, but the target's own `U` is free: a bidirectional unification §14.5 has no phase for | binds `T := Pair`, rejected at the formation by §9.7 | a new inference phase in §27.6 and §14.5 |
| **C** | **A reference to a generic method names its type arguments; the formation is a §14.1 site (SIMPLIFY C9) — adopted** | `xs.Select(Util.Id<int>)`; the bare `Util.Id` is an error naming the list | `Util.Keep<Pair>`, rejected at the formation by §9.7 as `Keep<Pair>(…)` would be | one §27.6 paragraph, one §14.1 entry, one clause in §22.12's syntactic rule |
| **D** | Reject every reference to a generic method | the idiom is unwritable | unwritable | one sentence |

**C over B.** B writes an inference rule for a case with zero measured users, and the one case where inference would carry its weight — a reference as an argument to a generic call — is exactly where it fails to be unification: the target's own parameters (`Select<U>`'s `U`) and the referenced method's (`Id<T>`'s `T`) must be solved together, a phase §14.5 does not have and §14.5's *Agreement* rule was written to avoid ("the predictable subset, stated fully", rationale §10). C reuses §14.5's explicit-list rule verbatim ("binds the declared type parameters in declaration order and must supply exactly as many arguments as the method declares type parameters … When a full explicit list is supplied, no inference runs") and makes the formation an ordinary instantiation. **C over D**: D removes a form §28.14 presupposes and that C# admits; C costs one list where a user wants the form.

### 2.2 For the receiver position (RC49)

| | Option | `Reg<Pair>.Put(new Pair(it))` | `A<T>.N := A<Array<T>>.N + 1` |
|---|---|---|---|
| **A** | **"A generic type is instantiated by being named" covers the receiver position; §14.1's list and §14.6's forms say so — adopted** | rejected: `Reg<Pair>` is named, and `Put`'s store is §9.7's collection bullet | an expanding §14.6 edge, rejected at `A` |
| **B** | The enumeration is the definition; a type reached only through its statics is never instantiated | admitted; the static keeps a `Pair` borrowing `it` | no edge; unbounded closure |

A is the only sound reading, it is what the compiler does (#3160), and it costs one clause.

### 2.3 For the extension block (RC21)

| | Option | `ps.KeepAll(sink)` at `T = Pair` | `IReadOnlyList<int?>.AsObjects` (formation) |
|---|---|---|---|
| **A** | The block has no listed instantiation; nothing is read (reading B) | admitted; boxed borrows stored | admitted; `int?` slots read as references |
| **B** | A third kind in §3.6 rule 2: a block parameter's use is checked at each call of a block member at the block arguments the receiver binds (#5761's recommendation) | rejected at the call | needs the formation clause of §2.1 separately |
| **C** | **Each block member is a generic method declaring the block's parameters first (SIMPLIFY C11, a desugaring); the method clause, the call site and the formation site apply verbatim — adopted** | rejected at the call | rejected at the formation (the qualifier binds the block arguments; a member's own list follows the name) |
| **D** | Remove generic extension blocks | — | — |

**C over B**: B adds a third clause that restates the method clause for one more declaration kind; C says the block's parameters *are* method-level parameters of each member, which is how §14.5 already treats them ("Extension-block-level parameters are inferred from the receiver first … the resulting substitution is in force while the method-level parameters are inferred") and how §26.5 step 2 binds them. No verdict differs between B and C at a call; C alone gives the formation its site for free. **D is refuted by reach**: 138 generic extension heads in 24 gtest files, 11 rule cases, 6 spec examples (SIMPLIFY §3).

---

## 3. The rule

### 3.1 §14.1 *Instantiation and its site*

▲ After "and a call to a generic member at explicit or inferred type arguments": "— a call of a generic extension block's member at the block arguments the receiver binds ([§26.4](#sec-generic-extensions)) and a method-reference formation over a generic member, or over a member of a generic extension block, at the type arguments the reference's list or its qualifier supplies ([§27.6](#sec-method-references)) included — and a generic type named in **receiver position** of a static member access or at the head of a member chain ([§22.12](#sec-static-members): `Reg<Pair>.Put(x)`, `Cache<string>.Default.Count`)".

### 3.2 §3.6 rule 2

▲ "a use of a generic **method's** own parameter — a generic extension block's parameters counted, for this rule, as parameters of each of its members ([§26.4](#sec-generic-extensions)) — is checked at each instantiation of the method, a call at explicit or inferred type arguments or a method-reference formation at its type arguments ([§27.6](#sec-method-references)) among them."

### 3.3 §26.4

▲ Replace the closing sentence: "Each member of a generic extension block is read, for [§3.6](#sec-derived-type-properties) rule 2, [§14.1](#sec-generic-declarations) and [§14.2](#sec-constraints), as a generic method that declares the block's parameters before its own — bound by the receiver at a call ([§26.5](#sec-resolution) step 2) and by the qualifier at a method-reference formation ([§27.6](#sec-method-references)), never by an explicit type-argument list, which supplies the member's own parameters only ([§14.5](#sec-type-argument-inference): `xs.OfType<int>()`, `IEnumerable<int>.Map<string>`): a requirement a member body carries on a block parameter is checked at each such call or formation as [§3.6](#sec-derived-type-properties) rule 2 checks a method's own parameter, through a chain where the receiver's argument is or contains a type parameter in scope, and at the body for a dispatched generic method calling it ([§14.2](#sec-dispatched-generic-methods)); the head itself carries no such requirement — each receiver the block applies to is an instantiation formed, and checked, where its type is named." §26.5.1's `First` annotation cites this sentence.

### 3.4 §27.6

▲ New paragraph after the first: "**References to generic methods.** A method reference over a **generic method** — one declaring type parameters of its own — names its type arguments on the member name (`Util.Keep<Pair>`, `s.Put<[2 owned]Item>`, `xs.Select(Util.Id<int>)`), under [§14.5](#sec-type-argument-inference)'s all-or-nothing rule for explicit lists; a reference to a generic method with no list is a compile error naming the type parameters to supply, and no inference binds them from the target. The formation is an **instantiation site** ([§14.1](#sec-generic-declarations)): [§3.6](#sec-derived-type-properties) rule 2's requirements, [§14.2](#sec-constraints)'s constraints, its dispatched-method exclusion and [§28.3](#sec-bare-type-parameter-returns)'s two per-instantiation exclusions (the `Task`-family exclusion and proposal 0165's *Exported mint slots*) are read at the formation exactly as at a call with that list, and never at an invocation through the formed value, which reads the function type alone. A reference over a member of a **generic extension block** binds the block's parameters by its qualifier (`IEnumerable<Pair>.KeepAll`) and the member's own by a list after the name (`IEnumerable<int>.Map<string>`); the qualifier's arguments are an instantiation of the block member in the same way ([§26.4](#sec-generic-extensions)). Overload selection follows [§14.5](#sec-type-argument-inference)'s list rule: with a list, only candidates declaring that many type parameters of their own take part; without one, only candidates declaring none take part, and the error above is reported where every candidate is generic (`var f: (int) -> int := Math.Abs;` selects among `Abs`'s non-generic overloads; `Math.Max` must be written `Math.Max<int>`)."

### 3.5 §22.12's syntactic rule and Appendix A.7

▲ "whose closing `>` is immediately followed by a token that can continue a primary expression — `.`, `?.`, `(`, or `[` — **or by a token that cannot begin an operand — `)`, `]`, `,`, or `;`** (which is how a method reference at explicit type arguments, `xs.Select(Util.Id<int>)` or `var k := Util.Keep<Pair>;`, parses as one, [§27.6](#sec-method-references); a relational `>` always has a right operand, so no relational reading is lost)." The two A.7 comments say the same, and the `PrimaryAtom` comment's "a bare `Identifier GenericArgs` is not a complete expression" becomes "is a complete expression only as a method reference at explicit type arguments" (critic fix: the draft left that sentence contradicting the new follow set).

### 3.6 §14.2, §14.5, §14.6, §28.14, Appendix C

- §14.2 *No move-only or inline-array substitution*: ▲ "A call of one at such an argument, **or a method-reference formation over one at such an argument ([§27.6](#sec-method-references))** — …".
- §14.5 phase 2: ▲ "A method-reference argument resolves against the substituted target per §27.6, **a reference to a generic method carrying its own type arguments, so that the candidate's signature is closed over them**; type parameters free in that target are bound by the winning candidate's signature …".
- §14.6: ▲ the request forms gain "a type name in receiver position of a static member access, and a method-reference formation at type arguments".
- §28.14: ▲ the example reads `var put: (ref () -> int) -> () := b.Put<() -> int>;`.
- Appendix C.1 and C.4: one bullet each (§5).

---

## 4. Soundness and consistency

**Soundness.** Every rule that reads a substitution reads it at a §14.1 site; the three places that formed a substitution outside the list now form one on it. For the formation, the explicit list makes the substitution the same one a call with that list would have, so every check that is sound at the call is sound at the formation, and the formed value's invocation adds nothing: it reads a function type whose slots are the substituted signature (the same argument §27.6 already makes for the conditionally-admitted register, v0.328, #5634). For the receiver position, the type is named, so §3.6 rule 2's "checked at every site that names it" applies on its own wording. For the block, the desugaring is literal: §14.5 and §26.5 step 2 already treat the block's parameters as method-level parameters bound first.

**Consistency.** The explicit list reuses §14.5's *Explicit lists are all-or-nothing* unchanged; the syntactic clause extends the §22.12 scan on its own ground (the scan "commits no semantic judgment", and no relational `>` can be followed by `)`, `]`, `,` or `;`); the §28.14 bar keeps its example with the list it always presupposed. `DECLINED.md` 233 (v0.328: a failing register condition is an error at the formation, never a candidate's drop-out) and rationale line 1506 ("the formation is the last site that sees the receiver's type arguments") point the same way.

**Simplicity.** Three clauses, one new paragraph, one deleted sentence; no inference phase; no keyword.

---

## 5. Normative edits

The exact `old → new` set is `scratch/v0334/W5.json` (ids `W5-0164-*`): §3.6 rule 2, §14.1, §14.2, §14.5, §14.6, §22.12, §26.4, §26.5.1, §27.6 (two edits), §28.14, Appendix A.7 (two comments), Appendix C.1 (one bullet: a method-reference formation over a generic member is an instantiation site), Appendix C.4 (one bullet: a generic extension block's parameters are each member's own), rationale §10 (one dated paragraph), the code-generation reference §3.3 (one row) and §7.4 (one pair), `DECLINED.md` (four rows), `README.md` (this proposal's row).

---

## 6. Declined alternatives

| Alternative | Why declined | Reopening bar |
|---|---|---|
| Binding a referenced generic method's own type parameters by unifying its declared signature against the target (option B, #5777 recommendation (a)) | A new inference phase for zero measured users; as an argument to a generic call it is a bidirectional unification §14.5's *Agreement* rule was written to avoid | a measured corpus in which references to generic methods are frequent enough that the list is a burden |
| Rejecting every method reference over a generic method (option D) | Removes a form §28.14 presupposes, for a saving of one sentence | —(none stated) |
| Reading §14.1's enumeration as the definition, so that a type reached only through its statics is never instantiated (#5784 reading B) | Unsound (the `Reg<Pair>` escape) and contrary to the compiler (#3160) | —(none stated) |
| A third clause in §3.6 rule 2 for block parameters (#5761's recommendation), or removing generic extension blocks | The desugaring states the same verdicts in the clause that exists; removal is refuted by 138 gtest heads, 11 rule cases and 6 spec examples | —(none stated) |

---

## 7. Migration and reach

Measured on `tests/` (gtest string programs, lit, the doc-example and rule corpora) and `docs/` at `429fd72` (`scratch/v0334/w5work/mref_generic.py`, SIMPLIFY's `inspect_c9_generic_method_refs.txt`, and `grep` for the receiver and extension forms).

- **Newly rejected (spec-ahead-of-impl):** a method reference over a generic method with no type-argument list. Sites: the §28.14 example `b.Put` (respelled `b.Put<() -> int>`) and rule case `s28_14_0025`, whose program gains the same list so the §28.14 bar stays the rule it pins (verdict `reject`, unchanged; no `baseline.tsv` row); and `array_unresolved_member_2457`'s `legit_bare_linq_select_take_first_now_rejected_2492` (`var f := a.Select;`, already rejected with the "not yet supported" diagnostic, which the implementing commit may re-word to this rule's — a diagnostic-text flip to grep). No other site (critic's scan `W5crit/mref_scan.py`: every occurrence, dotted or bare, of a name declared generic in the same program or in the stdlib, outside a call or an explicit list, in gtest, lit, both corpora and every document's code); `numbers.OrderBy(Math.Abs)` references a non-generic method and is untouched; `C.Apply(C.Five)` (#1073) likewise.
- **Newly rejected by reading A at the receiver position:** a generic type whose statics carry a §3.6 rule 2 requirement, named only in receiver position at an argument that fails it. Zero sites: 552 `G<Args>.Member` spellings in `tests/`, every one at an argument carrying no borrow and no move-only kind (`Result<int, string>.Ok(5)`, `GenericFactory<Target>.Make()` and their kin).
- **Newly rejected at an extension call (reading A, already presupposed):** zero sites among 138 generic extension heads (SIMPLIFY §C11: "all keep compiling").
- **Newly admitted:** the explicit-list spelling of a method reference over a generic method (nothing today parses it).
- **Doc-only:** §14.6's forms, §26.4's restatement, §26.5.1's citation, Appendix C.
- **Corpus:** `s28_14_0025` edited as above by the implementing commit (program and `rule:` header; `manifest.tsv` row's description unchanged). The doc-example corpus is unedited (#1380); the §26.5.1 comment edit changes one extracted fragment's comment text only (no verdict).
- **Guides:** none teaches a method reference over a generic method or a static access on a generic type; no pin moves.

---

## 8. Spec-ahead-of-impl

One `spec-tracking` issue at landing: (i) the parser's §22.12 scan admits `>` before `)`, `]`, `,`, `;` (`tokensLookLikeMethodTypeArgs`); (ii) a reference to a generic method requires its list and is formed at that substitution (`buildMethodRefType`), with §3.6 rule 2, §14.2, §28.3 and §28.14 read there; (iii) a block member's interior requirements read at each extension call and formation (the general rule-2 catch-up, cross-referenced); (iv) `s28_14_0025`'s respelling.

---

## 9. Composition

- W8 (RETRO R2) consolidates §28.3's function-value text; this proposal's §28.3 contact is the §28.14 example only.
- W1's option (a)/(a') deletes §9.7's borrow bullets family; the sites this proposal adds stay needed for every other requirement kind (§7.10, §9.2, §12.9, §15.1, §14.2).
- Proposal 0165 (this lane) edits §28.3's nested-slot paragraph and row 5; the two do not share an anchor.
