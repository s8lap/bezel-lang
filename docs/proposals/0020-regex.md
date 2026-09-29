# Proposal 0020 (RFC) — Regular expressions: engine family and the v1 surface

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target module** | `System.Text.RegularExpressions` (new; explicit `import`) |
| **Written against** | spec **v0.177** / stdlib **v0.165** |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); re-verified clean at v0.306 (2026-09-17 sweep) — §28.14's *call-bounded* parameter default, §29.11's `ToArray()` re-enumeration remedy, `Range<int>`, §10.5's enum restrictions and the `ReadOnlyStringSpan`/`ReadOnlySpan` type names all still read as cited. No tier-1 respells, no tier-2 flags. Written-against pin unchanged. |
| **Tracking issue** | [#1449](https://github.com/s8lap/bezel-lang/issues/1449) |
| **Gap-review item** | #19 (2026-07 ranking, wave D3) — *"Regex engine — P1; deferred in stdlib §11"* |
| **Design character** | **RFC** — the load-bearing decision (engine family) is a language-values question, not a mechanical one. This proposal *frames the fork and recommends*; the fork is now resolved under delegated ruling authority (see "Rulings recorded at acceptance" below). |

## Rulings recorded at acceptance

Resolving §11's open questions under delegated ruling authority (2026-07-07; owner review pending). The recommendations were the proposal's own; the reasoning is one line each.

1. **Engine family (Q1) → Option A, linear-time (RE2 / Thompson-NFA family).** It is the only choice consistent with Bezel's paid-for predictable-cost posture (§29.11 loud re-enumeration, §6.1 checked arithmetic, §2.18/§3.2 defined faults): a linear automaton makes ReDoS-immunity *structural*, not advisory. Backreferences/lookaround are recorded non-goals; any backtracking mode is a separate, explicitly-selected engine, never a silent default change.
2. **Match shape (Q2) → 1a (owned `sealed` class, borrow-free, group-bearing).** Borrow-freedom (integer `Range<int>` coordinates only) is the decisive property — no §9.7 lifetime coupling, no #1438/#1448 async hazard, and `sealed` + wholly-owned value state makes it sendable (§30.14 rules 1–2). Groups ship in v1; the pure-value-record 1b is declined as too thin (extraction-with-groups is most of regex's value).
3. **`\d \w \s` default (Q3) → ASCII by default, `\p{…}` for Unicode.** Bounds the shipped Unicode-table footprint (matching RE2/Rust-regex), with an explicit Unicode-aware growth seam.
4. **`\p{…}` subset (Q4) → `L N Nd Lu Ll P Z`.** `Nd` (decimal digit) is added to the recommended subset because it is the workhorse category; the rest stay as proposed.
5. **Named groups (Q5) → included in v1.** Cheap once the group-range array exists (§5) and high-value for extraction.
6. **Module name (Q6) → `System.Text.RegularExpressions`.** A namespace named after the type would give `System.Text.Regex.Regex`; the stdlib §11 forward-reference is updated when this lands.
7. **`RegexOptions` (Q7) → named-default bool constructor parameters, no flags enum.** Enums carry no `|` combination operator in v0.177 (§10.5; flags-enum operators are open RFC #1381), so a flags-style `RegexOptions` is inexpressible today; `ignoreCase`/`multiline`/`dotAll` bools (0019's Parse-knob pattern) combine freely. A flags model revisits if #1381 lands.

---

The stdlib names regex as a future addition (stdlib [§11](../Bezel_Standard_Library.md), "`System.Text.Json` and `System.Text.Regex` — a general JSON DOM/streaming API and regular expressions"). Nothing regex-shaped exists today. This proposal designs the v1 surface, but its centre of gravity is one owner question — **which engine family** — because that choice commits the language to a cost model, and cost predictability is a stated Bezel value (§29.11 made re-enumeration loud, checked arithmetic §6.1 refuses silent wraparound, stdlib §2.18/§3.2 make bad formats and bad encodings *fault* rather than paper over them). A regex engine is the first stdlib component where the "obvious" mainstream choice (backtracking) quietly imports a denial-of-service class the rest of the language went out of its way to design away.

---

## 1. What exists today (the substrate this complements)

Regex is not a replacement for the string surface — it is the tool for the jobs the string surface cannot express (extraction with alternation, repetition, capture). The v1 design deliberately leans on what already exists and only fills the genuine gap.

The current **`String`** surface (stdlib [§9.6](../Bezel_Standard_Library.md#sec-string)) already provides, and regex must **not** duplicate:

```bezel
// String (existing, stdlib §9.6) — the plain-substring surface
public method Contains(value: string): bool;
public method StartsWith(value: string): bool;
public method EndsWith(value: string): bool;
public method IndexOf(value: string): int;              // first occurrence, or -1
public method LastIndexOf(value: string): int;
public method Replace(oldValue: string, newValue: string): string;  // literal, all occurrences
public method Split(separator: string): owned Array<string>;         // literal separator
public method Split(separators: Array<char>): owned Array<string>;
public readonly property [r: Range<int>]: string;       // slicing by code-point range (§4.6)
```

**Design consequence:** a caller who needs a *literal* substring test, replace, or split already has it and should not reach for `Regex`. The regex surface (§4) mirrors these verbs — `IsMatch`/`Replace`/`Split` — but every one takes a *pattern*, and the naming keeps the distinction visible at the call site (`s.Contains("x")` vs `re.IsMatch(s)`).

Two more existing pieces are load-bearing for the ownership design:

- **`Range<int>`** (spec [§16.3](../Bezel_Language_Spec.md#sec-the-ranget-type)) — a compiler-intrinsic **value record** over integer indices. `string`'s slicing indexer `s[r: Range<int>]` (§9.6) turns a range into a substring. This is the pivot of the Match design (§5): a match is *coordinates*, and the string already knows how to be sliced by coordinates.
- **`ReadOnlyStringSpan`** (stdlib [§3.3](../Bezel_Standard_Library.md#sec-readonlystringspan)) — a **borrow-bearing record** (§9.7) viewing a contiguous run of a string. Ergonomic for zero-copy slicing, but *borrow-bearing*, and that property is exactly what the Match design must decide about (§5).

The **`String`** primitive is a sequence of **code points**: `Length` is the code-point count, `s[i]` is the i-th code point as a `char`, both O(1), with adaptive 1/2/4-byte internal storage (spec [§4.6](../Bezel_Language_Spec.md#sec-string)). This single fact drives the Unicode design (§8): the regex engine's coordinate system and the string's coordinate system are the same code-point axis, so `subject[m.Range]` is exact with no re-encoding.

---

## 2. THE OWNER QUESTION — engine family

This is the decision that must be made before anything else; the v1 surface (§3–§8) is written to be *engine-agnostic in its shape* but the feature list depends on the ruling.

### The fork

**Option A — linear-time family (RE2 / Thompson NFA + lazy DFA).** The pattern compiles to a finite automaton; matching is `O(n · m)` in subject length `n` and pattern size `m`, *guaranteed*, with no input able to trigger super-linear blowup. This is the RE2 / Rust-`regex` / Go-`regexp` lineage.
- **Cost:** no backreferences (`\1`), no arbitrary lookaround (`(?=…)`, `(?<=…)`). These are the features whose semantics *require* backtracking; a linear automaton cannot express them.
- **Gain:** a hostile or accidentally-pathological pattern-plus-input *cannot* melt a service. There is no ReDoS.

**Option B — backtracking family (PCRE / .NET / Java / PCRE2).** The full classical feature set: backreferences, lookahead, lookbehind, atomic groups, possessive quantifiers.
- **Cost:** worst-case **exponential** time on adversarial patterns (`(a+)+$` against `"aaaa…!"` is the canonical bomb). This is the ReDoS vulnerability class — a real, exploited, CVE-bearing category of denial-of-service.
- **Gain:** the features Option A lacks.

### Recommendation — Option A (linear-time), stated in Bezel's own terms

Recommend the **linear-time family** for v1. The argument is not "RE2 is a nicer library"; it is that **Option B contradicts a value the language has already paid for repeatedly**:

- §29.11 made lazy-query re-enumeration a **loud** compile error / `InvalidOperationException` rather than silently re-running work, *because* "predictable cost" beats "convenient surprise." A regex whose runtime is exponential in the input is the same surprise, one layer down.
- Checked arithmetic (§6.1) refuses to let an `int` silently wrap; the defined-fault encoding model (stdlib §3.2) refuses to silently substitute a replacement character; §2.18 faults on a bad format specifier. The through-line is: **the language does not ship footguns whose failure mode is invisible until production.** Catastrophic backtracking is precisely such a footgun — the pattern looks fine, the tests pass, and a crafted 30-character input hangs a core.
- Bezel is a *systems* language with **no GC and explicit resource discipline**. A component whose CPU cost is unbounded in its input, with no static signal, is out of character for a language that tracks every heap allocation's owner.

If Option B were chosen, the language would then have to *document the DoS away* — ship guidance ("don't write nested quantifiers," "set a timeout") that pushes a correctness obligation onto every caller. That is the opposite of the defined-fault posture. **The linear-time engine makes the guarantee structural instead of advisory.**

### The non-goals, recorded (not "missing features")

Under Option A, these are **deliberate non-goals for v1**, not oversights:

| Feature | Status | Reason |
|---|---|---|
| Backreferences (`\1`, `\k<name>`) | Non-goal | Requires backtracking; NP-hard in general. |
| Lookahead / lookbehind (`(?=)`, `(?!)`, `(?<=)`, `(?<!)`) | Non-goal | Requires backtracking (bounded lookaround is *implementable* in a DFA engine but adds real complexity; deferred). |
| Atomic groups / possessive quantifiers (`(?>…)`, `a++`) | Non-goal | Backtracking-control constructs — meaningless in a non-backtracking engine. |

### The escalation path (if evidence ever demands it)

If a real in-tree consumer later needs backreferences or lookbehind, the answer is **a separate, explicitly-selected engine mode** — `new Regex(pattern, RegexEngine.Backtracking)` or a distinct `BacktrackingRegex` type — **never a silent upgrade** of the default engine. The linear-time engine stays the default and the guarantee stays intact for everyone who did not opt in. This mirrors §29.11's discipline (you must *explicitly* `.ToArray()` to re-enumerate) and the escalation discipline of the whole stdlib: opt in to the sharper tool, don't get it by accident. This escalation is **out of scope for v1** and is recorded only so the door is a labelled door, not a hole.

---

## 3. Design questions and decisions

The remaining decisions are engine-independent in shape (they hold under either family) and are recommended here for the owner to confirm alongside the engine ruling.

### Q2 — the compiled-pattern object, and whether to cache

Compiling a pattern to an automaton is real work; doing it per-call in a loop is a silent cost. Two entry styles:

- **Compiled object** — `using var re := new Regex(pattern); … re.IsMatch(s) …`. The compilation cost is *visible* (there is an allocation, an owner, a `using`) and paid once. This is the primary, recommended form.
- **Static one-shots** — `Regex.IsMatch(subject, pattern)`. Convenient for a single use; **recompiles the pattern every call.**

**Decision — ship both, with no hidden cache.** The static conveniences exist (one-shot use is common and forcing a `using var` for it is ceremony), but they are documented as *recompiling*, and there is **no process-global pattern cache** behind them. A cache would be:
1. exactly the [#1363](https://github.com/s8lap/bezel-lang/issues/1363) static-mutable-state side door (a cross-domain mutable map), which has no sound design yet; and
2. a *hidden* cost/eviction policy — un-Bezel, the same reason the gap review declined an implicit format cache and normalization-in-equality.

The one-liner recompiles; the hot loop uses `new Regex` once. The cost is legible either way. (The gap review's declined-list entry on hidden caches, and the #1363 posture, are the precedent.)

### Q3 — result ownership: see §5 (the Bezel-specific crux, treated on its own).

### Q4 — pattern syntax subset: see §7.

### Q5 — Unicode / matching unit: see §8.

---

## 4. Proposed v1 surface

All items below are **proposed** (nothing here exists in the current stdlib). Module `System.Text.RegularExpressions`, **explicit `import`** (regex is a heavyweight opt-in tool, not part of the auto-imported `System.Text` core, and its failure type travels with it per the §31.4 built-in-vs-import principle).

```bezel
// System.Text.RegularExpressions  (PROPOSED, requires `import`)

/// A compiled pattern. Owned: compilation cost is explicit and the automaton
/// is released at scope end. Immutable and re-usable across many subjects.
/// The compiled Regex holds NO borrow of any subject — subjects are supplied
/// per call — so a Regex is not lifetime-coupled to any string.
public sealed class Regex
  /// Compiles `pattern`. Raises RegexParseException (with position) on a
  /// malformed pattern — a defined fault at construction (§7). Options are
  /// named-default bool parameters (Q7) — enums carry no `|` combination
  /// operator in v0.177 (§10.5; flags-enum operators are open RFC #1381), so a
  /// flags-style RegexOptions is inexpressible; the bool knobs mirror 0019's
  /// Parse-knob pattern and combine freely:
  ///   new Regex(pat, ignoreCase := true, dotAll := true)
  public constructor(pattern: string,
                     ignoreCase: bool := false,   // (?i) — ordinal simple case folding, matching §9.6's ignoreCase model
                     multiline: bool := false,    // (?m) — ^/$ match at line boundaries
                     dotAll: bool := false);      // (?s) — `.` also matches the newline

  public readonly property Pattern: string;
  public readonly property GroupCount: int;               // number of capturing groups (excl. group 0)

  /// True iff the pattern matches anywhere in `subject`. No allocation.
  public method IsMatch(subject: string): bool;

  /// The first (leftmost) match, or null if none. See §5 for the Match shape.
  public method Find(subject: string): owned Match?;

  /// Every non-overlapping match, left to right. Eager (NOT lazy) — see §6.
  /// OwningArray (not Array) because it OWNS the freshly-allocated Match
  /// instances (Match is an owned class, §5) and disposes them with it (§4.5,
  /// where T: class). Empty OwningArray when there are no matches.
  public method FindAll(subject: string): owned OwningArray<Match>;

  /// Replace every match with `replacement`, honoring $-substitutions (§4.1).
  /// Returns a fresh string; `subject` is unchanged (strings are immutable, §9.6).
  public method Replace(subject: string, replacement: string): string;

  /// Replace every match with the result of `replacer` applied to that match.
  /// `replacer` is a call-bounded (§28.14) callback: it is invoked once per
  /// match during the call and is never retained. It receives a BORROW of the
  /// match (valid only for that invocation) and returns the replacement text.
  public method Replace(subject: string, replacer: (Match) -> string): string;

  /// Split `subject` at every match; the matched separators are removed.
  /// Mirrors String.Split's return shape (§9.6): owned Array<string>.
  public method Split(subject: string): owned Array<string>;

  // --- static one-shots: convenient, RECOMPILE each call, NO cache (Q2) ---
  public static method IsMatch(subject: string, pattern: string): bool;
  public static method Find(subject: string, pattern: string): owned Match?;
  public static method FindAll(subject: string, pattern: string): owned OwningArray<Match>;
  public static method Replace(subject: string, pattern: string, replacement: string): string;
  public static method Split(subject: string, pattern: string): owned Array<string>;
end Regex;

/// A single match. BORROW-FREE: carries only integer ranges into the subject's
/// code-point coordinate system — no span, no string borrow (§5). Callers slice
/// the subject themselves: `subject[m.Range]`. Owned because it holds a small
/// owned Array<Range<int>> of group ranges (value elements). Sendable: `sealed`
/// (so §30.14 rule 1 admits it) with wholly-owned value state and no borrows
/// (rule 2 — a borrowing collection of sendable value elements), unlike a
/// span-carrier.
public sealed class Match
  public readonly property Range: Range<int>;             // group 0 — the whole match extent
  public readonly property GroupCount: int;               // capturing groups, excl. group 0

  /// Range of the i-th capturing group (0 = whole match). Null if the group
  /// exists in the pattern but did not participate in this match.
  public method Group(index: int): Range<int>?;
  /// Range of a named group (§7), or null if absent / non-participating.
  public method Group(name: string): Range<int>?;
end Match;

/// Compilation-time options are the three named-default bool constructor
/// parameters above (`ignoreCase`/`multiline`/`dotAll`); there is no
/// RegexOptions flags enum, because enums carry no `|` in v0.177 (§10.5, open
/// RFC #1381). Inline flags (§7) cover the per-subpattern cases.

/// Malformed-pattern fault, raised at Regex construction. Lives in this module
/// (its only producer is Regex construction — §31.4's import-with-producer rule)
/// and extends FormatException ("string does not conform to an expected format",
/// spec §31.4). Adds the position for a pointable diagnostic.
public class RegexParseException: FormatException
  public readonly property Index: int;                    // 0-based offset in the pattern
  public constructor(message: string, index: int);
end RegexParseException;
```

### 4.1 Replacement-string substitutions

`Replace(subject, replacement)` interprets these tokens in `replacement`; every other character (including `\`) is literal:

| Token | Expands to |
|---|---|
| `$0` or `$&` | the whole match |
| `$1` … `$n` | the n-th capturing group's text (empty if the group did not participate) |
| `${name}` | the named group's text |
| `$$` | a literal `$` |

An unknown `$`-token (e.g. `$9` when the pattern has 3 groups) is a **defined fault**: `RegexParseException` is raised when the replacement is parsed (at the `Replace` call), consistent with §7's malformed-input-faults rule. There is no silent "leave it as literal `$9`" behavior — that is the same defined-fault discipline as the rest of the surface.

---

## 5. Q3 — Match ownership: the Bezel-specific crux

This is the decision that has no analogue in a GC'd regex library, and it is the reason this proposal is more than a feature list.

### The hazard

A match "is" a region of the subject. The ergonomic encoding — the one every GC'd language uses — is to make a match carry the *text*, or a *view* of the text. In Bezel a text view is a **`ReadOnlyStringSpan`**, which is a **borrow-bearing record** (spec [§9.7](../Bezel_Language_Spec.md#sec-records-and-disposal); stdlib [§3.3](../Bezel_Standard_Library.md#sec-readonlystringspan)). A `Match` that carried a span would therefore:

1. be **lifetime-coupled to the subject** — the match may not outlive the string it views (§9.7's borrow-lifetime rules), so `FindAll` results could not be returned past the subject's scope, stored in a field, or collected freely;
2. inherit the **async borrow hazards** of [#1438](https://github.com/s8lap/bezel-lang/issues/1438) and [#1448](https://github.com/s8lap/bezel-lang/issues/1448): a borrow-bearing value held across an `await` has, today, *undefined* lifetime semantics — so `var m := re.Find(s); await …; use(m);` would be in exactly the unresolved territory those issues describe; and
3. be **non-sendable** — a borrow-bearing value cannot cross `Domain.Spawn` (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)), so matches could not be produced on one domain and consumed on another.

### The decision — a borrow-free Match carrying coordinates

**Recommend: `Match` carries no borrow of the subject at all. It carries integer `Range<int>` values — coordinates in the subject's code-point axis — and the caller slices the subject itself.**

```bezel
using var m := re.Find(subject);
if m is not null then
  var whole   := subject[m.Range];                // §9.6 slicing by Range<int> (m narrowed, §12.7)
  var g1range := m.Group(1);
  if g1range is not null then
    var g1 := subject[g1range];
  end;
end;
```

This dissolves all three hazards at once:
- **No §9.7 coupling** — a `Range<int>` is a plain value record; `Match` holds no reference into the string, so it may outlive the subject, be returned, stored, and collected freely. (`subject[m.Range]` would of course fault with `IndexOutOfRangeException` if applied to a *different, shorter* string — but that is an ordinary bounds fault, not undefined behavior.)
- **No #1438/#1448 async problem** — a `Match` held across `await` carries no borrow, so there is nothing whose lifetime is undefined. `var m := re.Find(s); await f(); g(subject[m.Range]);` is well-defined.
- **Sendable** (as a declared property) — a `Match`'s state is wholly-owned value data (integer ranges: `Array<Range<int>>` of sendable value elements), so the stdlib can declare it sendable across domains (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)'s wholly-owned-state criterion), where a span-carrier could never be.

The trade the caller accepts is one indexer call (`subject[m.Range]`) to turn coordinates into text — an O(k) copy for a k-length slice (§9.6). That copy is *explicit and legible*, which is the Bezel default; the zero-copy span form remains available to callers who want it and can satisfy its lifetime (`subject.AsSpan(m.Range)` — §9.6/§3.3 — a `ReadOnlyStringSpan` the caller constructs, and whose borrow-lifetime the caller then owns). The library does not *impose* the borrow; it lets the caller opt into it locally.

### Why `Match` is an owned class, not a pure value record

The task's ideal shape is a *pure value record* carrying the whole-match range **plus group ranges**. There is a hard constraint: capture groups are variable in number (per pattern), so their ranges need a heap collection (`Array<Range<int>>`), and **records cannot own heap resources** — §9.7 forbids a record from implementing `IDisposable`. So "pure value record carrying a dynamic group array" is not expressible.

The resolution keeps the *decisive* property (borrow-freedom) and relaxes only the incidental one (value-vs-owned):

- **Recommended (1a): `Match` is an owned class** owning a small `Array<Range<int>>` of group ranges (value elements — no per-element disposal). It is **borrow-free** (only ints), **sendable**, and disposed by `using`/auto-drop like any owned value. Groups are always available. The allocation is explicit — consistent with the whole-language posture on visible cost.
- **Alternative (1b): `Match` is a pure value record `(Range: Range<int>)` with no capture groups** — allocation-free, but groups deferred entirely. Weaker (extraction-with-groups is most of regex's value), recorded as the lighter option if the owner prefers to ship the barest core first.

**Recommend 1a.** It preserves the property that actually matters for Bezel (no borrows → no §9.7 coupling, no #1438/#1448, sendable) and pays only an explicit, ordinary allocation for it.

### Rejected: span-carrying Match

Recorded as considered-and-declined: `Match` carrying a `ReadOnlyStringSpan` (or two `int`s reinterpreted as a span). More ergonomic (`m.Value` gives text with no re-slice), but reintroduces all three hazards above. **Declined** — the ergonomic win is small (one indexer call) and the cost is a lifetime-coupled, async-unsafe, non-sendable result type. The whole point of doing this in Bezel rather than copying a GC'd design is to *not* make that trade.

---

## 6. Q3 cont. — `FindAll`: eager, not lazy (§29.11)

`FindAll` returns an **eager `owned OwningArray<Match>`**, not a lazy sequence. (`OwningArray`, not `Array`, because the collection *owns* the freshly-allocated `Match` instances and disposes them — stdlib §4.5, `where T: class`; §5 for why `Match` is an owned class.)

A lazy `IOwningEnumerable<Match>` (owned matches produced on demand during enumeration) would be a **lazy query** in the sense of spec [§29.11](../Bezel_Language_Spec.md#sec-multiple-enumeration): **single-consumption**. Enumerating it twice would be a compile error where visible, `InvalidOperationException` where only runtime-reachable. For a `FindAll` result that a caller naturally wants to `Count` *and* then loop over, that single-consumption footgun is a poor default. An eager materialized `OwningArray<Match>` is re-enumerable any number of times (§29.11's own remedy — "materialize first"), so the common "how many, and what are they" pattern just works.

The cost is that all matches are found up front even if the caller stops early. For v1 that predictability is the right default; a lazy `FindMatches(subject): owned IOwningEnumerable<Match>` form governed by §29.11's single-consumption rule is a *recorded possible addition* (§10), not part of v1. (This is the same eager/lazy call the gap review's LINQ items navigate, resolved the same way: default to the re-enumerable materialized form, offer laziness explicitly when asked.)

---

## 7. Q4 — the v1 pattern subset (the RE2 syntax)

v1 accepts the following constructs. A pattern using anything outside this set — or malformed within it — raises **`RegexParseException`** with the failing `Index` **at `new Regex(...)`** (or at the static call). Malformed patterns are a **construction-time defined fault**, never a match-time surprise and never a silent "treat the bad bit as a literal."

**Accepted in v1:**

| Category | Syntax |
|---|---|
| Literals | any non-metacharacter; `\`-escaped metacharacters (`\. \* \( \\` …) |
| Character class | `[abc]`, `[^abc]`, ranges `[a-z]`, combinations `[A-Za-z0-9_]` |
| Predefined classes | `\d \D`, `\w \W`, `\s \S` (ASCII semantics by default — see §8) |
| Any-char | `.` — one code point, excluding `\n` unless `DotAll`/`(?s)` |
| Anchors | `^` `$` (line-aware under `Multiline`/`(?m)`), `\b` `\B`, `\A` `\z` |
| Quantifiers | `*` `+` `?`, `{n}` `{n,}` `{n,m}`, and lazy forms `*?` `+?` `??` `{n,m}?` |
| Alternation | `a|b|c` |
| Grouping | capturing `(…)`, non-capturing `(?:…)`, named `(?<name>…)` (also accept `(?P<name>…)`) |
| Inline flags | `(?i)`, `(?m)`, `(?s)` — scoped `(?i:…)` forms accepted |
| Unicode property | `\p{L}`, `\p{N}`, `\p{Nd}`, `\p{Lu}`, `\p{Ll}`, `\p{P}`, `\p{Z}` (a **subset** of general categories — see §8) |

**Rejected in v1 (the Option-A non-goals, §2):** backreferences (`\1`, `\k<name>`), lookahead/lookbehind (`(?=)` `(?!)` `(?<=)` `(?<!)`), atomic groups / possessive quantifiers (`(?>…)`, `a++`). A pattern containing one of these is a `RegexParseException` naming the unsupported construct — not a silent mis-parse.

**Named groups: included in v1.** Once the group-range array exists (§5), exposing a name→index map is cheap, and named capture is high-value for extraction (`(?<year>\d{4})-(?<month>\d{2})`). `Match.Group(name)` returns the range. (Deferring named groups is a possible reduction if the owner wants v1 even smaller — recorded, not recommended.)

---

## 8. Q5 — Unicode: the matching unit is the code point

The one fact that makes this exact: a Bezel `string` is indexed by **code point**, O(1), with adaptive internal storage (spec [§4.6](../Bezel_Language_Spec.md#sec-string); stdlib §9.6). Therefore:

- **`.` matches one code point** (not one UTF-8 byte, not one grapheme cluster). Under default flags it excludes `\n`; `DotAll`/`(?s)` includes it.
- **Character classes and ranges are over code points.** `[a-z]` is the code-point range U+0061–U+007A; `[\u{1F600}-\u{1F64F}]` is a code-point range in the emoji block. Ranges compose with the same `Range<char>` semantics the language already defines (spec §16.3's `Range<char>` — note the surrogate gap U+D800–U+DFFF is not a valid `char`, so a class spanning it follows §16.3's surrogate-boundary rule).
- **Match indices are code-point indices.** A `Match.Range` is a `Range<int>` in the *same* coordinate system as `s[i]` and `s[r]`. This is the payoff of the borrow-free design: `subject[m.Range]` (§9.6 slicing) is exact and needs no byte↔code-point re-mapping, because the engine and the string agree on what "index 5" means. An engine that matched over UTF-8 *bytes* internally must translate its byte offsets to code-point offsets before returning a `Match` (an implementation obligation recorded here, invisible to callers).

**Grapheme clusters are explicitly not the unit.** `string` has no grapheme index (§4.6 is code-point-indexed), so `.` matching a single "user-perceived character" (e.g. an emoji with modifiers, or a combining sequence) is **out of scope** for v1. This matches every mainstream engine's default and is recorded as a non-goal, not a defect.

### The `\d \w \s` scope — ASCII by default, `\p{…}` for Unicode

There is a genuine size/behavior decision here, the same one the gap review flags for item #31 (Unicode normalization — "Unicode-table size decision"):

- **Recommend: `\d \w \s` are ASCII by default in v1.** `\d` = `[0-9]`, `\w` = `[A-Za-z0-9_]`, `\s` = `[ \t\n\r\f\v]`. Small, predictable, no large Unicode tables shipped in the compiler/runtime, and it is RE2's and Rust-`regex`'s well-trodden default (Unicode mode is opt-in there too).
- **Unicode categories are the explicit path:** `\p{L}` (any letter), `\p{N}` (any number), `\p{Nd}` (decimal digit — the workhorse), `\p{Lu}`/`\p{Ll}` (upper/lower letter), `\p{P}` (punctuation), `\p{Z}` (separator) — a **subset** of the full general-category set, sized to a small table. This keeps the Unicode-data footprint bounded while giving a real Unicode-aware path for callers who need it.
- A `(?u)` inline flag / a future Unicode-aware constructor bool that makes `\d \w \s` themselves Unicode-aware is a **recorded possible addition** (§10), gated on the same table-size decision as #31 — not v1.

Recording this as an owner-visible decision (rather than silently choosing) is the point: the ASCII-default keeps v1 small and the `\p{…}` subset is the growth seam.

---

## 9. Tour by example

```bezel
import System.Text.RegularExpressions;

// --- compiled, reused across many subjects (the hot-path form) ---
using var digits := new Regex(@"\d+");
for var line in lines do
  if digits.IsMatch(line) then Process(line); end;
end for;

// --- extraction via coordinates; the caller slices (§5) ---
var text := "released 2026-07-04 stable";
using var date := new Regex(@"(?<year>\d{4})-(?<month>\d{2})-(?<day>\d{2})");
using var m := date.Find(text);
if m is not null then
  var yr := m.Group("year");                    // m narrowed to Match (§12.7)
  if yr is not null then
    Console.WriteLine("year = " + text[yr]);    // slice the subject by Range<int>
  end;
end;

// --- all matches, eager and re-enumerable (§6) ---
using var words := new Regex(@"\w+");
using var all := words.FindAll(paragraph);
Console.WriteLine("count = " + all.Count.ToString());   // enumerate once…
for var w in all do Handle(paragraph[w.Range]); end for; // …and again — fine (eager)

// --- replacement string with $-substitution (§4.1) ---
using var swap := new Regex(@"(\w+)@(\w+)");
var masked := swap.Replace("a@b c@d", "$2#$1");         // "b#a d#c"

// --- callback replacement: call-bounded closure (§28.14) ---
var subject := "x1 y2";
using var num := new Regex(@"\d+");
var doubled := num.Replace(subject, lambda(match: Match) => do
  // `match` is a borrow, valid only for this invocation; not retained (§28.14).
  var n := int.Parse(subject[match.Range]);
  return (n * 2).ToString();
end);

// --- one-shot: convenient, recompiles, no cache (Q2) ---
if Regex.IsMatch(userInput, @"^[a-z0-9_]+$") then Accept(); end;
```

*(The examples use only cited existing APIs — `Console.WriteLine` (stdlib §2.1), `int.Parse` (stdlib §9.1), string slicing `s[r]` (stdlib §9.6 / spec §4.6), `.Count` and `for` iteration — plus the proposed regex surface. `@"…"` is the verbatim-string form, spec §18.6.)*

---

## 10. Recorded possible additions (NOT v1)

Kept explicitly out of v1 so the door is labelled:

- **Lazy `FindMatches(subject): owned IOwningEnumerable<Match>`** — the single-consumption (§29.11) lazy dual of `FindAll` (owned-Match elements → the owning enumerable family), for early-exit scans over huge inputs.
- **`(?u)` Unicode-aware `\d \w \s`** — gated on the same Unicode-table-size decision as gap #31.
- **A separate backtracking engine mode** (`RegexEngine.Backtracking`) — only if a real consumer needs backreferences/lookaround; an *explicit, opt-in* second engine, never a silent default change (§2).
- **`ReadOnlyStringSpan` overloads** (`IsMatch(subject: ReadOnlyStringSpan)`, matching over a span) — deferred until #1438/#1448 settle the borrow-across-await story, since a span subject reintroduces exactly that question on the *input* side.
- **Streaming/incremental match** over a `TextReader` (proposal 0006) — a large-input mode.

---

## 11. Open questions for the owner

1. **Engine family (§2)** — confirm **Option A (linear-time / RE2)** for v1, with backreferences/lookaround as recorded non-goals and any backtracking mode as a separate explicit engine. *This is the load-bearing ruling; everything else is downstream.*
2. **Match shape (§5)** — confirm **1a (owned, borrow-free, group-bearing)** over 1b (pure value record, no groups). Both are borrow-free; the choice is whether groups ship in v1.
3. **`\d \w \s` default (§8)** — confirm **ASCII-by-default** with a `\p{…}` subset as the Unicode path (bounds the Unicode-table footprint), vs. Unicode-by-default from the start.
4. **`\p{…}` category subset (§7/§8)** — confirm the small subset (`L N Nd Lu Ll P Z`) is enough for v1, or name the categories that must be in the first cut.
5. **Named groups in v1 (§7)** — confirm include (recommended) vs. defer for a smaller v1.
6. **Module name** — the stdlib §11 forward-reference calls the feature "`System.Text.Regex`". This proposal uses **`System.Text.RegularExpressions`** (a namespace named after the *type* would give `System.Text.Regex.Regex`). Confirm the namespace; minor, but it fixes the `import`.
7. **Options surface** — *resolved (ruling 7):* named-default bool constructor parameters (`ignoreCase`/`multiline`/`dotAll`), no flags enum, since enums carry no `|` in v0.177 (§10.5, open RFC #1381); inline flags (§7) cover per-subpattern combination. Revisit a flags-combination `RegexOptions` only if #1381 lands.

---

## 12. Relationship to existing issues and proposals

- **Gap-review item #19**, wave D3 ([stdlib gap review](stdlib-gap-review-2026-07.md); [design plan](stdlib-gap-design-plan-2026-07.md)). The review notes it as "deferred in stdlib §11" — this proposal is that deferral coming due.
- **[#1438](https://github.com/s8lap/bezel-lang/issues/1438) / [#1448](https://github.com/s8lap/bezel-lang/issues/1448)** — borrow-bearing values (params / locals) in async contexts. The borrow-free Match design (§5) is chosen *specifically* to stay clear of these; span-input/span-Match forms are deferred (§10) until they settle.
- **[#1363](https://github.com/s8lap/bezel-lang/issues/1363)** — static-mutable-state side door: the reason there is no hidden pattern cache (§Q2).
- **spec §9.7** (borrow-bearing records), **§16.3** (`Range<int>`), **§29.11** (lazy single-consumption), **§28.14** (call-bounded callbacks, v0.177), **§4.6** (string = code points), **§31.4** (exception hierarchy / import-with-producer principle); **stdlib §9.6** (String), **§3.3** (ReadOnlyStringSpan), **§2.18** (defined-fault format model), **§1** (Try convention — `Find` returns a nullable, the (b) nullable-return Try shape).
