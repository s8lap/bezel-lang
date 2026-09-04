# 0123 — Stdlib regularization batch, 2026-09: four regularizations, two removals, one generalization, three editorial items

| | |
|---|---|
| **Status** | under-review — draft (2026-09-04 elegance review docket). Drafted from review lane S3 and its adversarial verification V3 ([`elegance-review-2026-09.md`](elegance-review-2026-09.md)); every S3 count was re-measured for this draft because the lane's usage tables did not reproduce (see the review record's *what this pass teaches*). Companion to [0122](0122-language-simplification-batch-2026-09.md) (the language half of the same docket). |
| **Tracking issue** | not yet filed (filed at acceptance) |
| **Target module(s)** | **stdlib** — [§1](../Bezel_Standard_Library.md#sec-conventions) (two convention sentences, one table), [§2.1](../Bezel_Standard_Library.md#sec-console) (one sentence), [§2.2](../Bezel_Standard_Library.md#sec-math), [§2.13](../Bezel_Standard_Library.md#sec-the-type-class), [§2.15](../Bezel_Standard_Library.md#sec-concrete-serialization-formats), [§3.2](../Bezel_Standard_Library.md#sec-encoding), [§4.4](../Bezel_Standard_Library.md#sec-arrayt), [§4.14](../Bezel_Standard_Library.md#sec-owned-buffers-and-spans), [§4.15](../Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant), [§4.20](../Bezel_Standard_Library.md#sec-counterk), [§6.7](../Bezel_Standard_Library.md#sec-task-static-helpers), [§6.8](../Bezel_Standard_Library.md#sec-domain), [§7.2](../Bezel_Standard_Library.md#sec-filestream), [§7.4](../Bezel_Standard_Library.md#sec-textreader-textwriter-directory-path), [§8.1](../Bezel_Standard_Library.md#sec-stopwatch), [§8.2](../Bezel_Standard_Library.md#sec-debug), [§9.6](../Bezel_Standard_Library.md#sec-string). **spec** — [§17.10](../Bezel_Language_Spec.md#sec-the-typeof-operator)'s `Type` block (R6a only: one line) and one example word in §3.4 (R6g). **rationale** — one §5.15 cross-reference sentence (§5 item 1). **`Bezel_For_Code_Generation.md`** — every item that touches a signature moves its line (process §9 obligation; the lines are named per item). **`DECLINED.md`** — rows for the options not taken (§6). **Grammar untouched by every item.** Conformance corpus per item (process §6). |
| **Written against** | v0.286, HEAD `3f044f3`. Every `file:line` below was re-located with `sed -n` against that tree on 2026-09-04; every count was re-run with the recipe stated in §0. Compiler citations were obtained by reading `src/` only — nothing here required a build, and the one claim that would (R1's named-argument binder) is marked as a probe, not asserted. |
| **Currency** | **Quoted:** stdlib §1, §2.1, §2.2, §2.13, §2.15, §2.19, §3.2, §4.4, §4.8, §4.14, §4.15, §4.16, §4.20, §5, §6.6, §6.7, §6.8, §7.1, §7.2, §7.3, §7.4, §8.1, §8.2, §9.6; spec §5.1, §8.6, §17.5, §22.5, §22.6, §22.15, §26.2, §26.5; rationale §5.15, §15.x; changelog v0.57, v0.107, v0.108, v0.109, v0.165, v0.215, v0.222, v0.228, v0.282. **Cited without quotation:** spec §6.1, §12.7, §14.2, §26.1; stdlib §4.5, §9.1, §9.3. **Re-verify before graduation:** §4.14's extension blocks (R7 rewrites them), §1's Try bullet (R8 tabulates it), and the §22.6 tie-break paragraph (R1 leans on it). |
| **Origin** | The 2026-09-04 elegance and complexity review, lane S3 (the standard library, §1–§11 read end-to-end). The lane's pattern finding — that three mechanical multipliers inflate the surface without adding capability: the two-span-family mirror, overload pairs where §22.5 defaults already serve, and `To`/`From` duals — survived verification in its first two parts and was **refuted** in its third (§5 item 1). |

*Reference convention: unprefixed `§`-references are **stdlib** references at v0.286 (this is a stdlib proposal); **spec**, **rationale**, **changelog** and **process** references are marked as such. `:NNNN` alone is a line in `docs/Bezel_Standard_Library.md`; other files are named. Every quotation is verbatim from the v0.286 tree; emphasis inside a quotation is this proposal's unless the sentence says otherwise. ▲ marks proposed normative text, landed nowhere. Item IDs R1–R10 are the docket's fixed mapping and are the IDs the review record and 0122 use.*

**Grep convention (every count below).** `tests/` = `grep -rn --include=*.cpp --include=*.bzl -F '<literal>' tests/`; `docs/` = `grep -rn --include=*.md -F '<literal>' docs/` (all eleven documents **plus** `docs/proposals/`); `src/` = `grep -rn -F '<literal>' src/`; `corpus` = the same over `tests/conformance/`. Fixed-string (`-F`) throughout; where a regex was necessary it is shown in full. Counts are line hits at HEAD `3f044f3`, 2026-09-04. The lane's S3-11 table was produced with `grep -E` on patterns containing an unescaped `(` and is wrong by up to 8× in both directions; nothing from it is carried here unmeasured.

---

## 0. What this proposes, in one page

| ID | Item | Class (process §2) | Migration |
|---|---|---|---|
| **R1** | nine overload pairs become one `:=`-defaulted signature each | ruling (source-compatible) | zero call sites; one newly-defined call shape (`Binary.Parse<T>(arraySpan, ignoreUnknownFields := …)`) |
| **R2** | `Counter<K>.ContainsKey` → `Contains(key)`, so `key in tally` compiles | ruling | zero call sites (measured) |
| **R3** | drop the two array-family cross-mode `SequenceEqual` rows | design-change (removal, two rows) — editorial fallback offered | zero: cross-mode calls fall to the interface overload |
| **R4** | `Math.Sign` becomes one generic `Sign<T>` over `INumber<T>`; **`Abs` stays** | design-change (signature; `pure` lost) | zero measured; one newly-rejected shape (`char` argument, 0 sites) |
| **R5** | retire the windowed `Encoding.GetString(bytes, start, length)`; decide `String.From(buffer, start, length)` beside it (Option A/B) | design-change (removal) | 3 test call sites; `.AsReadOnlySpan(start, length)` |
| **R6** | four small .NET-heritage duals: `Stopwatch.Restart` (b), `String.IsNullOrEmpty` (g), `String.Compare` regularized with the #2581 gate carried (f), `Type.Implements` as a two-document removal (a) | design-change (removals); ruling (f) | 1 / 4 / 12 / 0 test sites respectively |
| **R7** | declare the byte read tier once, as `extension IReadOnlySpan<byte>` | design-change (declaration-only) | zero call sites; the `Span<byte>` delegating block disappears |
| **R8** | presentation: §4.14's double listing, §1's 517-word Try bullet, §6.8's 18 `Spawn` rows, §4.4's embedded issue text | editorial | doc-only |
| **R9** | the §1 recorded-misfit flag `TextReader.Read(): int` lacks | editorial | doc-only |
| **R10** | the unboxed-lowering sentence beside §2.1 `:111` | editorial (implementation guidance) | doc-only |

Net surface: R1 −13 declaration rows, R3 −2, R5 −1 (Option B) or −2 +1 (Option A), R6 −4 (+1 instance `CompareTo` overload), R7 −96 declared read members (128 → 32); R2 and R4 are renames in row count. What this batch does **not** do is in §5, with the record that stops each item — most importantly the refuted `CopyFrom`/`MoveFrom` deletion, whose recorded reason the lane missed.

---

## 1. Regularizations

### R1 — Nine overload pairs are default parameters in disguise

**What.** The document uses spec §22.5 defaults freely — `Increment(key: K, delta: int := 1)` (`:3456`), `FromForeign(p: IntPtr, length: int, freeOnDispose: bool := false)` (`:5462`), and **17** occurrences of `token: CancellationToken := CancellationToken.None` across §4.12, §4.13, §6.2, §6.3, §6.11, §7.1 and §7.3 — and declares *overload pairs* for the identical shape elsewhere:

| Member | Rows today | ▲ Defaulted form | Lines |
|---|---|---|---|
| `Task.Delay` | 4 | 2: `Delay(milliseconds: int, token: CancellationToken := CancellationToken.None): Task` and the `TimeSpan` twin | `:4173`–`:4176` |
| `Directory.Delete` | 2 | 1: `Delete(path: string, recursive: bool := false)` | `:4900`–`:4901` |
| `String.PadLeft` / `PadRight` / `PadCenter` | 6 | 3: `(totalWidth: int, paddingChar: char := ' ')` | `:5237`–`:5242` |
| `FileStream` constructor | 2 | 1: `(path: string, mode: FileMode, access: FileAccess := FileAccess.ReadWrite)` — the document already rules "The two-argument constructor implies **`FileAccess.ReadWrite`**" (`:4833`) | `:4817`–`:4818` |
| `BinaryDeserializer` ctor / `Binary.Parse<T>` / `Binary.ParseOwned<T>` | 3 / 3 / 3 | 2 / 2 / 2 with `ignoreUnknownFields: bool := false` | `:888`–`:891`, `:940`–`:949` |
| `Debug.Assert` | 2 | 1: `Assert(condition: bool, message: string := "")` — see the reading below | `:4955`–`:4956` |
| `Math.Round(double)` | 2 | 1: `Round(x: double, digits: int := 0)` | `:146`–`:147` |
| `String.IndexOf` (char / string) | 4 | 2: `(value, startIndex: int := 0)` | `:5208`–`:5211` |
| `Array<T>.IndexOf` | 2 | 1: `(item: T, startIndex: int := 0)` | `:1571`–`:1576` |

Thirteen rows go; every parameter keeps its current name (`token`, `recursive`, `paddingChar`, `access`, `ignoreUnknownFields`, `message`, `digits`, `startIndex`), because §1's window convention makes names API surface ("parameter names are API surface (named arguments, spec §22.5)", `:66`).

**The rule it leans on, quoted.** Spec §22.5 *Permitted default-value expressions* (`Bezel_Language_Spec.md:4687`–`:4689`): a default may "be a literal, constant, or `default(T)`; call static methods, including factory methods that return owned instances; read other static state". Every default in the table is a literal (`false`, `' '`, `""`, `0`) or a constant (`FileAccess.ReadWrite`, `CancellationToken.None` — the latter already admitted 17 times in this document). Spec §22.6's defaulted-arity tie-break (`:4719`, `:4732`–`:4739`) states the compatibility argument in so many words: "adding an optional-parameter overload never re-routes existing calls". This proposal *replaces* a pair with a single candidate rather than adding one, so no call can re-route at all; `Task.Delay(5)` and `Task.Delay(ts)` still separate by parameter type, and §1's own caveat — "Stdlib pairs do not declare a default value on the marked position (the two-defaults-filled ambiguity of spec §22.6 would make the argument-omitting call unwritable)" (`:41`) — is about `owned`/borrow duals on one arity and touches none of these rows.

**The `Debug.Assert` reading (the one substantive gap, settled).** `:4961` says a failed assertion "reports the condition's source location and the message"; today `Assert(condition)` reports no message. Two readings:

- **Reading 1 (recommended):** `message: string := ""`, plus one ▲ sentence at `:4961`: "an empty message is reported as no message". This is what the runtime already does: `bz_debug_assert_fail` prints `Assertion failed: <msg>` only when the message has `len > 0`, and falls through to the bare `Assertion failed` line for a null **or empty** string (`src/runtime/bezel_runtime_time.cpp:940`–`:963`; the one-argument arity passes a null pointer, `src/codegen/LlvmBackend_Calls.cpp:10959`–`:10963`). Observably neutral with a one-sentence contract.
- **Reading 2:** `message: string? := null` — exactly neutral with no new sentence, at the cost of a nullable parameter minted purely to encode absence, on a member whose only other parameter is a `bool`.

Reading 1 keeps the parameter non-nullable and matches what `Debug.Assert(false, "")` already does; Reading 2 buys nothing the runtime does not already give.

**Why it is complexity.** Two idioms for one shape in one document; each pair is an extra row for the reader and, per proposal 0062's own text, a drift: 0062 §1 item 5 proposed `String.PadCenter(width, padChar := ' ')` (`0062-stdlib-ergonomics-batch.md:15`) and it landed at v0.215 as two overloads with no stated reason for the split (changelog `:6984`). The `BinaryDeserializer` row also closes a gap the document records against itself — "no (ReadOnlyArraySpan<byte>, ignoreUnknownFields) form: the facade mirrors the BinaryDeserializer constructor set above, which declares no such constructor" (`:943`–`:945`) — for free.

**Record check.** `DECLINED.md:406` is the only default-parameter decline (*One-shot serialization facade alternatives* — "options as defaulted parameters"), and its stated reason is specific to a record-typed option with "no `JsonOptions` default constant to name"; every row above *has* a nameable default, so the decline's own ground does not reach them. No rationale entry on overload-vs-default. Changelog `:6984` (v0.215) records `PadCenter` landing as a pair with no reason.

**Usage (re-measured).** `Task.Delay(<int literal>` → **373** tests / 0 docs; `Task.Delay(TimeSpan` → 4 / 3; two-argument `Delay` (regex `Task\.Delay\([^,()]+,[^)]*\)`) → 17 / 1 — the token default is the win; both duration forms stay. `Pad*(` one-argument → 27 / 6, two-argument → 10 / 5. `new FileStream(` two-argument → 26 / 41, three-argument → 5 / 3. `Math.Round(` → 16 / 5, of which two-argument 5 / 2. `Debug.Assert(` → 6 / 6 lines, the executable call sites being three in `tests/codegen/diagnostics_646_test.cpp` (`:203`, `:210` with a message, `:214`) and Guide 06 `:272` (with a message; mirrored into `guide6_0012.bzl`). `Directory.Delete(` → **0** in `tests/`, **2** in `docs/` — both `docs/proposals/0017-temp-files-path.md:192`, `:202`, both the recursive form; the lane's "zero anywhere" was wrong, the conclusion (a default costs nothing) stands. Two-argument `IndexOf` calls across the string/array/span receivers → 22 in `tests/`, all positional. `new BinaryDeserializer(` two-argument → 1; `Binary.Parse*` with the bool → 0. All resolve unchanged.

**Corpus accounting.** The affected declaration blocks are transcribed into `stdlib_0116`/`stdlib_0117` (`Delay`), `stdlib_0147` (`Pad*`, string `IndexOf`), `stdlib_0138` + `codegen_0031` (`Directory.Delete`), `codegen_0031` + `stdlib_0135` (`FileStream`), `stdlib_0140` (`Debug`), `stdlib_0001` (`Round`), `codegen_0021` + `stdlib_0048` (`Array.IndexOf`), `stdlib_0026`–`stdlib_0029` (`ignoreUnknownFields`). Each re-transcribes by hand with its block (#1380); no exercised case converts, because no call shape is removed.

**Compiler.** Doc-only for the built-in arms, which are name-keyed and already accept both arities (`src/sema/ExprTypecheck_Calls.cpp:8590` — "Assert has two arities"; `:8055` — "The four Delay overloads"). One spec-ahead-of-impl delta: the newly-defined `Binary.Parse<T>(ReadOnlyArraySpan<byte>, ignoreUnknownFields := …)` shape (one issue). One probe, not asserted: named arguments on built-ins bind through `common::builtinParamNameCandidates` (`src/common/BuiltinRegistry.cpp:1298`, #3398); whether that table can carry a *default* — so that `Task.Delay(5, token := t)` and `Task.Delay(5)` are one row rather than two — is for the landing to check, and it changes nothing observable either way.

**Code-generation reference.** `:1317` already spells the pad family as `PadLeft/PadRight/PadCenter(totalWidth: int[, paddingChar: char])` and `:1346` writes `new FileStream(path, mode[, access])` — the reference has been reading these as optional parameters for some time; `:1291` (`Debug`) and the `Task.Delay` index line move to the `:=` spelling.

**Change class:** ruling. **Migration:** newly-defined — the `Binary.Parse<T>`/`ParseOwned<T>`/`BinaryDeserializer` array-span form with `ignoreUnknownFields`; everything else doc-only.

### R2 — `Counter<K>` spells key membership `ContainsKey`, so `key in tally` does not compile

**What.** Three maps carry both names — `Dictionary<K, V>` (`:1946`/`:1947`: "`Contains(key: K): bool;            // alias for ContainsKey; enables `key in dict`"), `OwningDictionary<K, V>` (`:2033`/`:2034`), `SortedDictionary<K, V>` (`:3309`/`:3310`) — and §4.8's prose states the purpose: "`Contains(key)` is an alias for `ContainsKey(key)`, enabling the `in` operator (language spec §17.5) to test key membership" (`:1989`, late in the iteration paragraph). `Counter<K>` (v0.215) carries **only** `ContainsKey(key: K): bool;            // distinguishes present-at-zero from absent` (`:3460`; the whole block `:3452`–`:3466` declares no `Contains`).

**The rule it leans on, quoted.** Spec §17.5: "`x in collection` evaluates to `true` if `collection.Contains(x)` is true. The right operand must expose a `Contains(T): bool` method (instance or extension)" (`Bezel_Language_Spec.md:3401`), and the lookup is structural — "any accessible method named `Contains` taking one parameter of compatible type and returning `bool` qualifies" (`:3415`). So `k in tally` has nothing to bind to: a compile error on the newest map-like type, from a reader who learned `key in dict` in §4.8.

**Proposed shape.**

```bezel
// ▲ §4.20, replacing :3460
  public method Contains(key: K): bool;              // distinguishes present-at-zero from absent;
                                                     //   enables `key in tally` (spec §17.5)
```

plus one ▲ sentence in §1: "Map-like types spell key membership `Contains(key)` so that `key in m` resolves (spec §17.5); `ContainsKey` on the hash and sorted dictionaries is the inherited .NET alias (rationale §15.x) and is not minted on new types." Deleting `ContainsKey` from the three dictionaries is a §15.x question (inherited familiarity) and is **not** proposed.

**Record check.** `DECLINED.md`: no row by either name. Rationale §15.x (`Bezel_Design_Rationale.md:2038`) tolerates inherited duplicates on a muscle-memory ground — "deleting would break arriving muscle memory for zero space savings" — and it is about `Convert`, i.e. *inherited* .NET surface; `Counter<K>` has no .NET antecedent, so §15.x does not reach it, and 0074 §7 states the boundary: reasoning "that extends no license to a *new* duplicate, which serves no arriving muscle memory" (`0074-stdlib-ergonomics-batch-3.md:160`). The v0.215 entry (changelog `:6984`) records `ContainsKey`'s *semantics* ("present-at-zero vs absent"), never the name against the `in` operator.

**Usage.** `ContainsKey(` → **81** tests / **18** docs / 21 src; `ContainsKey` on a `Counter` receiver → **0** (`grep -rn -F 'ContainsKey' tests/ | grep -i counter`); `Counter<` → 160 lines in `tests/`, so the type is exercised and the rename is free. Docs: Guide 02 `:97` and spec `:4815`, `:9770` are dictionary receivers and stand.

**Corpus accounting.** `stdlib_0090`, `stdlib_0091` transcribe the `Counter<K>` block (declaration mirrors; re-transcribe by hand). Code-generation reference `:1038` (`method ContainsKey(key: K): bool;` inside the `Counter<K>` block at `:1034`) moves.

**Compiler.** `"ContainsKey"` is name-keyed in three files (9 lines); whether the `Counter` arm is among them is for the landing — either way the spec-ahead-of-impl delta is one issue (`Contains` on `Counter`, and `in` binding to it).

**Change class:** ruling. **Migration:** newly-rejected — `tally.ContainsKey(k)` (0 sites); newly-defined — `k in tally`.

### R3 — `SequenceEqual`: the array family declares the cross-mode concrete overload the buffer family says is unnecessary

**What.** `ArraySpan<T>` declares three (`:3113`–`:3115`: same-type, `ReadOnlyArraySpan<T>`, interface) and `ReadOnlyArraySpan<T>` three (`:3177`–`:3179`), with the justification "the `ReadOnlyArraySpan<T>` overload supports comparison across the access-mode boundary without requiring an `AsReadOnly()` call at the comparison site" (`:3140`). The buffer family rules the opposite for the same question: "There is no *concrete* overload for `SequenceEqual(other: ReadOnlySpan<T>)`, but none is needed: the v0.108 interface form `SequenceEqual(other: IReadOnlySpan<T?>)` above admits a `ReadOnlySpan<T>` (and any other span family) directly, under the call-bounded boxing of language spec §8.6" (`:2690`).

**Framing, corrected by the verifier.** This is **not** two contradictory rulings. Changelog `:3958` shows the array spans shipped at v0.107 with "`SequenceEqual` (own type and cross-access-mode)" *before* `IReadOnlySpan` existed (v0.108, `:4000`), and v0.108 added the interface overload to every span "without retiring" the concretes — "The existing concrete-typed `SequenceEqual` overloads are unchanged" (`:4026`). So `:3140` is a post-hoc explanation of a legacy residue, and `:2690` is the rule the family adopted once the interface existed. Legacy residue is easier to land than a contradiction, and the change must not claim to resolve one that was never authored.

**Proposed shape.** Apply `:2690`'s rule to §4.15: delete `:3114` and `:3178`; every span type then declares exactly {same-type concrete, interface}. The type-level check passes: both array spans implement `IReadOnlySpan<T>` at the *same* element type (changelog `:4022`/`:4023`), so a cross-mode call falls to the interface overload with none of the nullability mismatch rationale `:627` warns about ("There is no implicit conversion across nullability or element types"). §4.16's own worked `bufSpan.SequenceEqual(arrSpan)` (`:3287`) already goes through the interface. **Editorial fallback, if the concrete cross-mode path is preferred:** one sentence in §4.14 beside `:2690` recording that the array family keeps its v0.107 concrete for direct dispatch. Either ends the asymmetry; the removal is recommended because the interface form is the recorded cross-source path and the cost — one virtual indexer read per element on the argument side — is the cost §4.14 already accepted.

**Record check.** `DECLINED.md`: no row. Rationale: none beyond `:629`'s statement that `SequenceEqual` *is* on the interface. Proposals: 0038 `:65` confirms the buffer family's minimality for the retired ascii span only.

**Usage.** `SequenceEqual(` → **53** tests / 36 docs / 33 src; with a `ReadOnly*`/`AsReadOnly()` argument → **23** (regex `SequenceEqual\([^)]*(ReadOnly|AsReadOnly\(\))`); the overload each resolves to is not grep-distinguishable, and every one resolves under the proposed surface by ordinary applicability. **Corpus:** `stdlib_0081`, `stdlib_0082` transcribe the two blocks (declaration mirrors); 29 `SequenceEqual(` lines in the corpus overall, none of which a removed row uniquely serves. Code-generation reference `:982`/`:992` list `SequenceEqual` without enumerating overloads and stand.

**Change class:** design-change (removal, two rows) — or editorial (the fallback). **Migration:** none; doc-only at every call site.

### R4 — `Math.Sign` becomes one generic; `Math.Abs` stays

**What.** `:157`–`:160` declare `Sign(x: int)`, `Sign(x: long)`, `Sign(x: double)`, `Sign(x: decimal): int` (the last "v0.228"), all `pure`; `:125`–`:128` declare the same four `Abs` forms. §5 records the principle that retired `Sum`'s per-type overloads: "The per-type overloads predated the §2.19 abstraction, and their recorded justification — 'no generic numeric constraint in this version' — had been false since v0.176" (`:3608`); `Sign(decimal)` was *added* at v0.228 as a symmetry completion (changelog `:7164`) fifty-two revisions after `INumber<T>` shipped.

**Proposed shape.**

```bezel
// ▲ §2.2, replacing :157–:160
  public static method Sign<T>(x: T): int where T: INumber<T>;    // -1, 0, or 1: two `<` tests against T.Zero()
```

The body uses only `INumber<T>`'s `Zero()` and `<` (`:1115`, `:1121`). The return type is `int` at every substitution, so no widening-dependent call changes meaning.

**Why `Abs` stays — the §5.1 trace the lane omitted.** All eleven numeric primitives conform to `INumber` (`:1135`), and spec §5.1's table admits `short → int`, `sbyte → int`, `float → double` implicitly (`Bezel_Language_Spec.md:670`–`:679`). So *today* `Math.Abs(shortVal)` resolves to `Abs(x: int): int` and `Math.Abs(floatVal)` to `Abs(x: double): double`; under `Abs<T>(x: T): T` those calls would infer `T = short` / `T = float` and the **return type changes** — visible at every `var`-bound result and every checked follow-on (`short` accumulation is checked at `short` width, spec §6.1). The lane's "silently incomplete — no `Abs`/`Sign` for `short`, `sbyte`, `float`" is false for the same reason: those calls compile today by widening. `Abs` is therefore not call-site-invisible and is **not** proposed (§5 item 2).

**The `pure` cost, stated.** Every per-type `Sign` is `pure`; the already-generic `Min`/`Max`/`Clamp` beside them (`:130`–`:132`) are not, because a `pure` body's promise covers "every member it transitively calls" (spec `:5406`) and `INumber<T>`'s operators are not `pure`-declared (`:1114`–`:1122`) — spec §22.15 admits `pure` on "static interface members" (`:5411`), but §2.19 does not yet spell it. So `Sign<T>` moves from the pure column to the non-pure column, matching the class's other generic members. Measured dependence: `grep -rnE '(requires|ensures)\([^)]*Math\.(Abs|Sign)' tests/ docs/` → **0**. The alternative — marking `INumber<T>`'s `Zero`/`One`/operators `pure` first — is a one-line widening that would also restore `pure` to `Min`/`Max`/`Clamp`'s siblings; it is recorded here as the follow-on, not folded in.

**One newly-rejected shape, honestly.** `char` converts implicitly to `int` (spec §5.1, `:679`) but does not conform to `INumber` (`:1135`), so `Math.Sign(someChar)` compiles today and would not under the generic. Sites: **0** (regex over every `Math.Sign(` argument in `tests/`; the 16 hits are `int`, `long` and `double` arguments only).

**Record check.** `DECLINED.md`: no row on `Abs`/`Sign`. Rationale: none. 0067 §2 added `Sign(decimal)` as "a twin `Abs` has but `Sign` lacks" without asking whether the family should be generic.

**Usage.** `Math.Sign(` → **16** tests / 0 docs (`tests/codegen/math_sign_test.cpp` and one line of `stdlib_v0284_removals_3559_test.cpp`); `Math.Abs(` → 17 / 2 (untouched). `src/`: `"Sign"` is name-keyed in two files. **Corpus:** `stdlib_0001` transcribes the `Math` block. Code-generation reference: no `Math.Sign` line today; the §2.2 index line moves.

**Change class:** design-change (signature; `pure` lost). **Migration:** newly-rejected — a `char` argument (0 sites); doc-only otherwise.

---

## 2. Removals

### R5 — Retire the windowed `Encoding.GetString(bytes: Array<byte>, start, length)`, and decide `String.From(buffer, start, length)` in the same breath

**What.** `:1325` `public abstract method GetString(bytes: Array<byte>, start: int, length: int): string;   // params renamed v0.282 (§1 window convention)` sits beside `:1328` `public abstract method GetString(bytes: ReadOnlyArraySpan<byte>): string;  // v0.222 (#1809): Array-backed twin (#1358)`. The document states the subsumption itself: the span-source overloads are "the same operation over a caller-selected range spelled as a span — identical fault model, BOM consumed only at the start of the span" (`:1356`), and §4.14's convention is "**No offset parameter.** Every method operates from the start of the span. To read at an offset, slice first" (`:2904`). The replacement spelling is real: `Array<T>.AsReadOnlySpan(start: int, length: int): ReadOnlyArraySpan<T>` (`:1596`). The §1 `Stream` carve (`:66`) exists because *async* stream I/O cannot take spans; `Encoding` has no async surface.

**Why it is complexity.** A third decode spelling for the array source where the library's convention is slice-then-call; the v0.282 window-parameter batch spent a rename on this very signature (`0119-stdlib-symmetry-batch.md:40`) instead of noticing it had been subsumed at v0.222 — that batch inspected parameter *names* only. Incidental defect (a) of the review — the stale "The `index`/`count` overload of `GetString`" prose at `:1356` — dies with the row.

**The sibling the lane missed.** `String.From(buffer: Array<char>, start: int, length: int): string` (`:5187`) is the exact structural sibling — a static decode-shaped operation over an `Array` window — and it has **no** span twin and no full-array overload: there is no `From(chars: ReadOnlyArraySpan<char>)`, and `ReadOnlyStringSpan` is string-backed (`:1364`). It would become the lone windowed non-stream member the moment `GetString`'s window is deleted. Two options:

- **Option A — retire both, adding the missing twin.** Delete `:1325` and `:5187`; add ▲ `public static pure method From(chars: ReadOnlyArraySpan<char>): string;` so `String.From(buf.AsReadOnlySpan(s, n))` spells the window. Net −2 +1 rows; **9** `String.From` window call sites migrate (`tests/codegen/stdlib_654_bool_bitconv_tochararray_test.cpp:336,347,357,367,378,417`, `stdlib_string_static_factories_leak_2228_test.cpp:133`, `string_from_ffi_buffer_2734_test.cpp:456`, and the named-argument form in `builtin_named_arg_binding_3398_test.cpp:448`) plus two corpus declarations (`stdlib_0147:9`, `codegen_0032:4`) and a runtime/codegen arm (`src/runtime/bezel_runtime_strings.cpp:647`, `src/codegen/LlvmBackend_Calls2.cpp:697`). A regularization batch that mints a member to make a deletion possible.
- **Option B (recommended) — retire `GetString`'s window only; record `String.From`'s.** Delete `:1325`; add one ▲ clause to §1's window paragraph at `:66`: "`String.From(buffer, start, length)` keeps its window because `Array<char>` has no span-shaped route to `string`; a span twin, if wanted, retires the window with it." One row, three call sites, and the exception is written where the convention is.

Option B is recommended because it is the subtraction the evidence earns; Option A is the right follow-on the day a char-span decode has a client.

**Record check.** `DECLINED.md`: none. Rationale: none. Changelog: the v0.222 entry (`:7066`) adds the span twins "on both byte-span families per the #1358 mirroring precedent" without retiring the window; v0.282 (`:8619`) renames it "rename-only, no arity or semantics change".

**Usage (re-measured; the lane's "8 of 35" did not reproduce).** `GetString(` → **50** tests / **12** docs / 36 src. Three-argument executable call sites: **3** — `tests/codegen/encoding_getbytecount_span_1355_stdlib_3_2_test.cpp:190`, `:203` and `tests/codegen/encoding_test.cpp:253`; the remaining regex hits are comments, the signature itself (`:66`, `:1325`), the code-generation reference `:775`, changelog `:8619` and 0119 `:40`. Migration: `enc.GetString(b, 1, 2)` → `enc.GetString(b.AsReadOnlySpan(1, 2))`.

**Corpus accounting.** `stdlib_0043.bzl:4` transcribes the `Encoding` block (declaration mirror; re-transcribe by hand). Under Option A add `stdlib_0147`, `codegen_0032`.

**Compiler.** The windowed decode lives in four runtime arms (`src/runtime/bezel_runtime_encoding.cpp:151`, `:461`, `:550`, `:641`) plus the name-keyed sema/codegen arms; removal is an impl edit in the same landing or the document asserts an absence the compiler contradicts (process §5 item 3b).

**Change class:** design-change (removal). **Migration:** newly-rejected — three-argument `GetString` (3 sites); under Option A also three-argument `String.From` (9 sites); newly-defined under Option A — `String.From(ReadOnlyArraySpan<char>)`.

### R6 — Four small .NET-heritage duals

The lane's usage table was wrong (unescaped `(` under `grep -E`); every figure here is re-measured with `-F`. Verdicts survive at (a), (b), (f), (g); (c), (d), (e) are keeps and are in §5.

| # | Member | Duplicate of | tests / docs / src | corpus | Verdict |
|---|---|---|---|---|---|
| (a) | `Type.Implements(iface)` (`:598`; spec `:3550`) | `iface.IsAssignableFrom(t)` — `:620`: "`a.IsAssignableFrom(b)` is the reflective form of the `is` operator" | **3 / 4 / 0** | 3 declaration files | remove — **two documents** |
| (b) | `Stopwatch.Restart()` (`:4942`) | `Reset(); Start();` — bare listing, no prose | **1 / 2 / 1** | `stdlib_0139` | remove |
| (f) | `String.Compare(a, b)` / `(a, b, ignoreCase)` static (`:5264`–`:5265`) | `a.CompareTo(b)` (the `IComparable<string>` conformance, `:5179`) | **12 / 0 / 4** | `stdlib_0147` | regularize: one instance `CompareTo(other: string, ignoreCase: bool)` beside the existing instance `Equals(other: string?, ignoreCase: bool)` (`:5269`) |
| (g) | `String.IsNullOrEmpty(value: string?)` (`:5199`) | `value?.IsEmpty ?? true` — `IsEmpty` at `:5181` (v0.231) | **6 / 3 / 1** | `stdlib_0147`, `codegen_0032` | remove; `IsNullOrWhiteSpace` stays (`String` has no instance `IsWhiteSpace`; only `Char.IsWhiteSpace(c)` at `:5152`) |

**(a) `Type.Implements` — zero call sites, but not the one-row deletion the lane described.** Its three `tests/` hits are conformance declarations (`stdlib_0014.bzl:24`, `stdlib_0015.bzl:24`, `spec_0093.bzl:16`), and its four `docs/` hits are the two stdlib lines, the code-generation reference `:708`, and **the language spec itself**: `Bezel_Language_Spec.md:3550` declares `Implements` inside §17.10's normative `Type` block (`:3541`–`:3557`). Call sites: **0** anywhere. `src/`: **0** — and the whole lattice is unimplemented (`grep -rn '"BaseType"\|"IsClass"\|"IsEnum"\|"EnumValues"\|"FullName"\|"IsAssignableFrom"\|"Implements"' src/` → 0), so there is no arm to delete and no absence to mis-assert. Landing: one line in each of three documents plus three corpus re-transcriptions. The review record files the lattice's missing honesty marker separately (its incidental (c)); this item does not depend on it.

**(b) `Stopwatch.Restart`** — one test site (`tests/codegen/diagnostics_646_test.cpp:140`), one runtime entry (`src/runtime/bezel_runtime_time.cpp:887` `bz_stopwatch_restart`) plus one name-keyed codegen arm, and two `docs/` mentions: `0106-language-lessons-survey.md:247` (a prose count) and **`0016-monotonic-time.md:148`, a live under-review proposal whose example calls `sw.Restart()`** — that example is edited when this lands (a proposal is not normative, but its example must not teach a retired member). Code-generation reference `:1291` moves. Migration: `sw.Restart()` → `sw.Reset(); sw.Start();`.

**(f) `String.Compare` — the removal is not free, and the gate must travel.** `src/sema/ExprTypecheck_Calls.cpp:2117`–`:2140` is a dedicated #2581 arm: the static callee "carries no Primitive-String VALUE exprType", so the §7 assignability check against the declared non-nullable `string` had to be added by hand for `Compare`, and `tests/sema/builtin_span_nullable_arg_2581_test.cpp` pins it (10 lines naming `string.Compare`, asserting on diagnostic text). `src/codegen/LlvmBackend_Calls2.cpp:813` and `:822` are two codegen arms. The proposed instance `CompareTo(other: string, ignoreCase: bool)` declares a non-nullable `other`, so an un-narrowed `string?` argument must be rejected by the *instance* gate (#2315's primitive-string receiver check) — the landing carries the #2581 test over to the new spelling rather than deleting it, or #2581 regresses silently. Migration: `string.Compare(a, b)` → `a.CompareTo(b)`; `string.Compare(a, b, true)` → `a.CompareTo(b, ignoreCase := true)` — 12 lines in two test files (`string_methods_test.cpp`, the #2581 test).

**(g) `String.IsNullOrEmpty`** — four executable test sites (`integration_test.cpp:331`, `:343`; the #2581 test `:294`, `:304`), two corpus declarations, one codegen arm (`LlvmBackend_Calls2.cpp:732`), and — a hit the lane did not list — **spec `:488`** uses `string.IsNullOrEmpty` as its example of static-member access spelling ("`int.Parse`, `int.MaxValue`, `double.IsNaN`, `string.IsNullOrEmpty`"); that word respells to `string.IsNullOrWhiteSpace` or `string.Join`. Code-generation reference `:1310` moves. Migration: `string.IsNullOrEmpty(s)` → `s?.IsEmpty ?? true` (or `s = null or s.IsEmpty`).

**Record check (all four).** `DECLINED.md`: no row by any of the names. Rationale §15.x tolerates *inherited* duplicates on the muscle-memory ground (`:2038`); 0074 §7 measured that ground for `Guid.NewGuid` at "38 in-tree occurrences" (`0074:161`). By that bar (a) and (b) clear easily, (g) clears (four call sites), and (f) is a regularization rather than a loss — the ignore-case capability survives as an instance form matching `Equals`.

**Change class:** design-change (a, b, g: removals); ruling (f). **Migration:** newly-rejected — `Implements` (0 sites), `Restart` (1), `IsNullOrEmpty` (4), static `Compare` (12); newly-defined — `CompareTo(other, ignoreCase)`.

---

## 3. Generalization

### R7 — Declare the byte read tier once, on `IReadOnlySpan<byte>`

**What.** §4.14 declares the endianness-aware read tier as `public extension ReadOnlySpan<byte>` (`:2823`, `:2917`), re-declares it on `Span<byte>` by delegation (`:2867`–`:2875`: "// ... (full set of 16 Read methods and 16 TryRead methods, each / delegating identically to the corresponding AsReadOnly method; / omitted here for brevity)"), and §4.15 mirrors it again: "**Byte-buffer accessors (v0.165, #1358):** the §4.14 endianness-aware extension family is mirrored onto this family — all `Read*{Little,Big}Endian`/`TryRead*` forms on `ReadOnlyArraySpan<byte>` and `ArraySpan<byte>`, and the `Write*`/`TryWrite*` forms on `ArraySpan<byte>`, with identical semantics" (`:3070`). The document already owns the unifying abstraction: `IReadOnlySpan<TElement>` is "An interface unifying the read-only access surface of every span type in the standard library. Implemented by all five concrete span types" (`:3242`). No `extension IReadOnlySpan` exists today (0 hits).

**Arithmetic, corrected.** Read tier: 16 `Read*` + 16 `TryRead*` = 32 members × four byte spans = **128 declared members that are one operation each**. Write tier: 16 `Write*` + 16 `TryWrite*` = 32 per writable span (`:2877`–`:2898`) × two writable spans = **64**, not the lane's 128. The write tier is untouched by this item.

**The mirror is non-uniform, and worse than the lane said.** Twinned across both families: `Encoding.GetString` (`:1327`/`:1328`), the `JsonDeserializer`/`BinaryDeserializer` constructors (`:858`–`:863`, `:888`–`:891`), `Json.Parse`/`ParseOwned`, `Binary.Parse`/`ParseOwned` (`:920`–`:949`), the byte accessors, the bit tier (`:3202`–`:3224`). Not twinned: `Stream.Read(buffer: Span<byte>)` / `Write(buffer: ReadOnlySpan<byte>)` (`:4777`/`:4778`), `Encoding.GetBytes(s, destination: Span<byte>)` (`:1330`), and `File.WriteAllBytes`, which declares only `(path, Array<byte>)` and `(path, ReadOnlySpan<byte>)` (`:4847`/`:4848`) — **no windowed form at all**, so a window over an `Array<byte>` has no route to it whatsoever (`Stream` at least has `(buffer, offset, count)`, `:4775`/`:4776`).

**Proposed shape.** One extension block replacing the four read-tier blocks:

```bezel
// ▲ §4.14 — one block; the receiver is any of the four byte spans
public extension IReadOnlySpan<byte>
  public method ReadInt16LittleEndian(): short;
  // … the 16 Read* forms
  public method TryReadInt32LittleEndian(out value: int): bool;
  // … the 16 TryRead* forms
end;
```

**Mechanism, verified against three spec rules.** (1) Spec §26.2 *What can be extended* admits "Interfaces" in its closed list (`Bezel_Language_Spec.md:5889`–`:5893`; the exclusions are type parameters, nullable types, function types, choices, inline arrays), and the precedent is this document's own §5 — `public extension IEnumerable<T>` at `:3492`, `:3579`, `:3616`, `:3627`, and `IOrderedEnumerable<T>` at `:3570`. (2) Spec §26.5 rule 2 searches "extension blocks on `x`'s type, base types, or implemented interfaces" (`:5954`), so a `ReadOnlySpan<byte>` receiver reaches the block; rule 1 does not block, because no span declares an instance member of these names. (3) Spec §8.6 sanctions the *receiver* box, not only the argument box: "a borrow-bearing record may be boxed for a short-lived interface call … provided the boxed value does not escape the borrow's source scope" (`:1259`) — the read accessors return values and retain nothing. The document already relies on exactly this at `:2690` for `SequenceEqual`, and §4.14's worked `ReadInt32LittleEndian` body (`:2918`–`:2926`) reads only `this.Length` and `this[i]`, both on the interface. `ReadOnlyStringSpan` is not admitted by accident (it implements `IReadOnlySpan<char>`, `:3268`), and the type that *did* implement `IReadOnlySpan<byte>` besides the four — `ReadOnlyAsciiByteSpan` (changelog `:4025`) — was retired at v0.188 (`:6664`/`:6670`), so the block's reach is exactly the four byte spans.

**What stays per-type.** The write tier (no writable interface — §4.16's recorded shape), so `Write*`/`TryWrite*` remain on `Span<byte>` and `ArraySpan<byte>`; the bit tier's records *store* their span in a field (`ArrayBitReader(Source: ReadOnlyArraySpan<byte>, …)`, `:3202`), which call-bounded boxing does not admit, so `BitReader`/`ArrayBitReader` stay mirrored. The same move gives `Stream.Write(buffer: IReadOnlySpan<byte>)` and `File.WriteAllBytes(path, contents: IReadOnlySpan<byte>)` array-window reach; whether to take that in the same revision is the owner's call (it is additive, so it is offered, not bundled).

**The record that must be named.** `DECLINED.md:287`, *Pure-body `for` over `IReadOnlySpan<TElement>`* (`deferred`): "its lowering is index-shaped but through interface-dispatched accessors not yet `pure`-declared". The byte accessors are not `pure` today (`:2869`, `:2918` carry no `pure`), so nothing is lost now; but the move pins the whole byte read tier behind that deferred contract question if the family is ever made `pure`. Stated here so the deferral's owner (proposal 0085) sees the new dependent. Otherwise: no `DECLINED.md` row on the mirror or on interface-declared accessors (rows exist only for *offset parameters* and *mixed bit-order families*, `:82`); rationale §5.15 `:629` constrains the interface's *member* surface ("deliberately minimal: indexer-get, `Length`, `IsEmpty`, `IndexOf`, `Contains`, and `SequenceEqual`"), which an extension block does not touch; the #1358 "mirroring precedent" is cited in the v0.165 batch line (changelog `:6016`) with no reason beyond closing the file→parse path. The in-document counter-consideration is §4.16's performance paragraph — "Indexer access through an `IReadOnlySpan<TElement>` parameter is a virtual dispatch" (`:3276`), "Authors of performance-sensitive APIs typically provide the concrete-type signatures" (`:3283`) — advice to API authors, not a ruling that the stdlib's own byte tier must be concrete; `:70` frees implementations to optimise, and the project's stated priority is simplicity first.

**Usage.** Accessor calls in `tests/` (regex `\.(Try)?(Read|Write)(U?Int(16|32|64)|Float|Double)(Little|Big)Endian\(`) → **33**: 27 in `tests/codegen/arrayspan_byte_accessors_1358_stdlib_4_15_test.cpp`, 3 in `deserializer_arrayspan_1358_stdlib_2_15_test.cpp`, 1 in `span_slice_one_arg_2312_test.cpp`, 2 in `tests/conformance/cases/stdlib_0076.bzl`; array-backed receivers (`(AsReadOnlySpan|AsSpan)\([^)]*\)\.…Endian`) → **16**. The compiler already treats the four receivers as one family: `src/sema/ExprTypecheck_Calls.cpp:9351`'s "documented surface" diagnostic names `Read*/TryRead*/Write*/TryWrite*{…}{LittleEndian,BigEndian} (stdlib §4.14/§4.15)` for every span kind in one arm; the document is the only place the family is spelled four times. Migration: **zero** — `bytes.AsReadOnlySpan().ReadUInt32LittleEndian()` (`:3070`'s own example) resolves unchanged.

**Corpus accounting.** `stdlib_0076`, `stdlib_0077`, `stdlib_0078`, `stdlib_0080`, `stdlib_0083` transcribe the extension blocks; re-transcribe by hand. `tests/conformance/test_conformance.py:104` synthesises `public extension ReadOnlySpan<byte>` as a harness fixture (unaffected — it declares its own member). Code-generation reference `:1008` ("mirrored on `ReadOnlyArraySpan<byte>`/`ArraySpan<byte>`") moves.

**Risk.** Devirtualisation of the indexer inside the accessor bodies becomes an optimisation the compiler may perform when the static receiver is concrete, rather than a property of the declaration — nothing in the language forbids it.

**Change class:** design-change (declaration-only; zero call-site migration). **Migration:** doc-only.

---

## 4. Editorial

### R8 — Presentation (four measurements, all reproduced)

- **§4.14 prints three types twice.** `:2438`–`:3063` is **626 lines**; `OwningBuffer<T>`, `Span<T>` and `ReadOnlySpan<T>` each appear as a class-T block and a value-T block, after which `:2526` says "The two presentations are different specializations of the same generic class." The category-dependent surface is a five-row table (slot type, setter modifier, `move` admissibility, bulk-mutation set, `IEnumerable` conformance). Proposed: one declaration per type with the category noted per member, plus one "Slot semantics by element category" table; the anchors `sec-owned-buffers-and-spans` and the sub-anchors survive.
- **§1's Try-convention bullet is one 517-word paragraph** (`sed -n '38p' | wc -w` → **517**; §1 over `:20`–`:71` is 3,341 words, so the bullet is a sixth of the section) naming four shapes, their false-path assignments and their member lists inline, then restated in the `*OrNull` sentence and the failure-signal rule. Proposed: a four-row table (shape · signature · false-path assignment · members) with the shape-(d) sentence beneath.
- **§6.8 `Domain.Spawn` lists 18 rows** (`:4336`–`:4377`) that factor exactly as {no arg, `owned` arg, value arg} × {no result, class result, value result} × {sync, async}; the prose already says so — "The nine async rows mirror the nine synchronous rows" (`:4384`). Proposed: the nine synchronous rows plus one sentence for the async twin. (The 19th `Spawn` in the file, `:4536`, is `TaskScope.Spawn`.)
- **§4.4's normative listing embeds compiler state** (`:1621`–`:1629`): "Three sema sites admit it positively (the §22.5 owning-container allow-list names `Sort`; …) … NOTE: the `T: IComparable<T>` gate is `Array`-only in the compiler today … #3325". Issue text, stale the day #3325 closes. Proposed: keep the one-line conditional-admission sentence with its spec §14.2 citation; move the rest to #3325.

**Record check.** The *Ownership-kernel consolidation form* decline in `DECLINED.md` rejects a *rewrite*, not a tabulation. **Change class:** editorial; process §7's history-narration rule is the authority for the §4.4 half.

### R9 — The recorded-misfit flag `TextReader.Read(): int` is owed

**What.** `:4871` `public abstract method Read(): int;             // next char as int, or -1 at EOF`, while §2.1 spells the identical condition on the same input as `ReadKeyOrNull(): char?` — "resolving the misfit the failure-signal rule recorded at v0.229: end of input is band 2's expected absence, and `char?` is exactly the `ReadLine` null-at-EOF analog" (`:113`). §1's rule requires a landed cross-band spelling to be flagged: "an existing spelling stands until its surface is re-ruled, and a landed spelling the rule would place in a different band is a recorded misfit — flagged where it stands, moved only by its own graduation ruling — never a conformance error" (`:48`). §7.4 carries no flag.

**Proposed shape.** One ▲ sentence at `:4871`: "Recorded misfit (§1's failure-signal rule): band 2 would spell this `char?`; the sentinel stands per proposal 0006's ruling until its own graduation re-rules it — compare `Console.ReadKeyOrNull` (§2.1)." The return-type change itself is **not** proposed (§5 item 4).

**Usage.** `.Read()` → 28 lines in `tests/`+`docs/`, none a `TextReader` call (user-declared `Read()` fixtures; the one `TextReader`-shaped mention is prose in `0006-stream-family.md:180`); `TextReader` in `src/` → 1 (`ExprTypecheck_Calls.cpp:19841`, the `Console.In` property). Corpus: `stdlib_0137`, `codegen_0031` transcribe the block (a comment change re-transcribes). *Could not verify:* V3 cites a code-generation-reference sentence at `:1287` ("The only concrete `TextReader`/`TextWriter` instances are what `Console.In`/`Out`/`Error` return"); that line in the v0.286 tree is the `IOException` hierarchy sentence and the quoted text appears nowhere in the file — the blast-radius claim rests on `:113`, `:4871` and `src/` alone.

**Change class:** editorial. **Migration:** doc-only.

### R10 — The unboxed-lowering sentence beside `:111`

**What.** §2.1 already records one compiler-known resolution rule in prose: "The `Object` overloads invoke `ToString()`. For interpolated strings, the compiler resolves the `string` overload directly" (`:111`). The per-type `Write`/`WriteLine`/`Append` families on `Console` (`:94`–`:96`), `StringBuilder` (`:1294`–`:1297`) and `TextWriter` (`:4884`–`:4886`) each state their content as "behavior = Write(value.ToString()); no Object boxing on the path".

**Proposed shape.** One ▲ sentence after `:111`: "A call to the `Object` overload whose argument is statically of primitive type is lowered without boxing (implementation guidance, not observable)." It records the precedent the family already relies on and costs nothing; the family's *removal* is not proposed (§5 item 3). `src/sema/ExprTypecheck_Calls.cpp:3764` keys the `Console` arm on the member name, so the sentence describes an obligation on codegen, not sema.

**Usage.** Identifier-argument `Console.Write*` in `tests/` (regex `Console\.Write(Line)?\([A-Za-z_][A-Za-z0-9_]*\)`) → **380**; literal `int`/`bool` argument → 8. All unchanged. **Change class:** editorial. **Migration:** doc-only.

---

## 5. Explicitly not proposed (so this batch's silence is not re-read as endorsement-by-omission)

1. **S3-03 — deleting `CopyFrom`/`MoveFrom` in favour of the `To` forms: REFUTED.** The lane wrote "Rationale: no entry." Rationale §5.15's titled paragraph *Ergonomics pass (v0.109)* (`Bezel_Design_Rationale.md:635`–`:648`) rules it directly: "Each is equivalent to the corresponding CopyFrom/MoveFrom on the receiving side — `source.CopyTo(dest)` is `dest.CopyFrom(source)`. **Both directions are provided because the natural call form depends on which side the caller holds.** Tight processing loops typically write `dest.CopyFrom(source)` when the destination is the new local being filled; pipeline-style code typically writes `source.CopyTo(dest)` when chaining transformations left-to-right. Neither form is strictly necessary; both reduce friction." (`:648`). Changelog `:4071` repeats it ("both are provided because the natural call form depends on which side the caller holds"), `:4086` counts the addition knowingly ("All have direct equivalents already expressible via existing surface"), and `:4111` records the family as *deliberately* asymmetric already ("`Span<T>` for class T does not get `CopyTo` (only `MoveTo`)"). The lane also read §1's no-dual rule outside its stated scope — it opens "One primary mechanism per fault condition" (`:58`) and `CopyFrom` signals no fault — and inverted its own quote (the *newer* spelling is `CopyTo`/`MoveTo`). Per process §2 this is settled; no new evidence against the recorded reason was offered. **What survives:** one editorial cross-reference from §4.14 to rationale §5.15 so a reader meeting four names finds the reason in place — carried in this batch's rationale/§4.14 edit list, not as an item.
2. **S3-06 `Math.Abs` generic** — not call-site-invisible (R4's §5.1 trace); a per-width return-type change with the `pure` loss, which this evidence does not earn.
3. **S3-07 removing the 60 per-type `Write`/`WriteLine`/`Append` rows** — the count is 60 (`:1297` enumerates `bool` + eleven numerics), not 55; `:70` disclaims normativity for performance *characteristics* and makes signatures normative, so it does not forbid performance-motivated surface (read that way it would condemn `TryWrite*` and `File.WriteAllBytes(path, ReadOnlySpan<byte>)` too); two adjacent declines in graduated 0074 take the family as load-bearing (`0074:160` `Console.ErrorLine`, `:164` `Append(Object)`); and the lane's proposed keep-set named a `Console.Write(char)` that does not exist (`:89`–`:93`). R10 is the additive half.
4. **S3-08 changing `TextReader.Read()` to `char?`** — `:48` grandfathers the landed spelling ("moved only by its own graduation ruling"); 0006 `:182` is an accepted proposal's explicit ruling on this member ("EOF stays a sentinel — `null`/`−1`/`0` — exactly as §7 already establishes"); and the v0.231 precedent was an *addition* (`ReadKeyOrNull` beside a still-throwing `ReadKey`, `:100`/`:101`), not a return-type change. R9 is the editorial half.
5. **S3-11 keeps:** (c) `Stopwatch.ElapsedMilliseconds` — 19 / 6 / 7, a measured client, a §15.x-class inherited alias; (d) `CancellationTokenSource.IsCancellationRequested` — the reason is recorded in place (`:3738`: "The duplication on the API surface matches C#'s analogous shape and exists for ergonomics"); (e) `Decimal.Zero`/`Decimal.One` — the lane's "3–5" is **21** in `tests/` (`Decimal.Zero` 5, `Decimal.One` 9, `decimal.Zero` 3, `decimal.One` 4), which by 0074's 38-occurrence bar no longer separates them from (c); the `int.Zero` asymmetry is real (only `:5103`/`:5104` declare such constants) and is recorded as a §15.x-class alias pending a re-measure.

---

## 6. Landing checklist (process §9, the parts this batch owes)

- **Change classes:** R1, R2, R6(f) ruling; R3, R4, R5, R6(a)(b)(g), R7 design-change (this proposal is their §6 gate); R8, R9, R10 editorial. One revision; the composition gate runs over the union (R5 and R7 both edit §3.2/§4.14/§4.15 prose; R1 and R8 both edit §1; R6(a) edits the spec beside 0122's items).
- **Corpus:** every case named per item above, re-transcribed by hand (#1380); no exercised case converts except the R5/R6 migrations listed.
- **Code-generation reference:** `:775`, `:1008`, `:1034`–`:1040`, `:1291`, `:1306` (Option A only), `:1310`, `:1317`, `:1346`, the `Type` line at `:708`, and the §2.2/§6.7 index lines — moved in the same commit, pin bumped.
- **`DECLINED.md` rows:** R3's fallback if the removal is declined; R5's Option not taken (with the `String.From` reason either way); R6 (c)(d)(e) keeps with their counts; the `Abs` decline (R4) with its §5.1 reason; the S3-07 and S3-08 declines with the records above. S3-03 needs no row — the rationale is the record.
- **Spec-ahead-of-impl issues:** R1's `Binary.Parse<T>` array-span `ignoreUnknownFields` shape; R2's `Contains`/`in` on `Counter`; R4's generic `Sign`; R6(f)'s `CompareTo(other, ignoreCase)` with the #2581 gate; R7's interface block (the name-keyed arms already cover the four receivers — verify, do not assume). **Impl edits in the same landing** (process §5 item 3b, or the docs assert an absence the compiler contradicts): R5's four runtime decode arms; R6(b)'s `bz_stopwatch_restart`; R6(g)'s `IsNullOrEmpty` arm; R6(f)'s two `Compare` codegen arms.
- **Guides:** no guide teaches a member this batch removes (`Restart`, `IsNullOrEmpty`, `string.Compare`, `Implements`, windowed `GetString` → 0 guide hits each); Guide 06 `:272` and Guide 02 `:97` stand. No pin moves unless R8's §1 table changes an idiom sentence a guide quotes (verify at landing).
