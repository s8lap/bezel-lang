# Proposal 0173 — A function value's hash agrees with its equality; the function type stays not hashable

| | |
|---|---|
| **Status** | graduated-v0.335 — superseded — graduated v0.335 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues", the 2026-10-09 direction "resolve all findings autonomously" and the 2026-10-09 "Please proceed as you recommend" after the v0.334 matrix re-runs: reading 1 of #5792 — a function value's `GetHashCode` is derived from exactly what §27.7.1's equality compares (a lambda from its closure object's identity hash; a method reference from its method and, where the equality compares a receiver, that receiver's identity hash), so §8.3's invariant holds for every type; §3.6 clause (e) keeps the function type out of the hashable set because a lambda equals only itself; proposal 0161's option-B1 row annotated, not struck; critique round 1 applied, shared with proposal 0172, which derived the hash sentence from the equality's comparison so that it holds under either reading of §27.7.1's extension-reference bullet, and dropped a no-collision promise §8.3 does not make; landed with the v0.335 changelog entry) |
| **Tracking issue** | [#5792](https://github.com/s8lap/bezel-lang/issues/5792) (`spec-question`: a function value held at `Object` is a hash key whose hash disagrees with its equality); one `spec-tracking` compiler tracker is filed at landing (§8) |
| **Target module(s)** | **spec**: §27.7.1 (the hash sentence), §3.6 (the hashable row's clause (e)), §8.3 (one table row), Appendix C.1 (one clause). **rationale**: §7.7 (one dated paragraph). **code-generation reference**: §7.2 (one comment). **`DECLINED.md`**: two `declined-at-proposal` rows, and the 0161 option-B1 row annotated |
| **Written against** | spec/stdlib **v0.334** at `fd8a9a2` (main). Section numbers are v0.334's. |
| **Origin** | The generics matrix's re-run against v0.334, root NR10 (cells R2/C5 and R2/C12; lane L3), consolidated in `scratch/matrix2/ROOTS2.md`, over the open #5792 (filed by v0.334 lane W2's critique of proposal 0161). It needs a proposal because it adopts the hash definition proposal 0161 §9 declined as option B1 ("a hash for a key nobody uses"), on new evidence against that reason: the key is used at `Object`. |

*Reference convention: unprefixed §-references are language-spec references at v0.334; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text.*

---

## 0. Summary

Proposal 0161 (v0.334) made a function type not **hashable** (§3.6 clause (e)) because "a function value's `GetHashCode` is identity while §27.7.1's equality compares a method reference by method and receiver", and wrote into §27.7.1 that the hash "need not agree with the equality above". That removed the function *type* as a key and left the function *value* a key: a capture-free function value is admitted at `Object` (§28.3 *Set-erasing positions*), `Object` is hashable, and §8.3 states "`GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`" for every type. So `Set<Object>` holds two evaluations of `Demo.Twice` — equal by §27.7.1 — as one element or two by bucket placement (R2/C12), and a generic `Agree<T>(a: T, b: T): bool where T: hashable, notnull => not (a = b) or a.GetHashCode() = b.GetHashCode()` is `false` at `Agree<Object>(f, g)` (R2/C5): a generic body relying on the documented invariant at a legal instantiation is wrong. The stdlib's stability rule keeps the containers memory-safe, so the defect is verdict-only.

The decision is #5792's reading 1: define the hash on the equality's own key. A lambda hashes by the identity hash of its closure object; a method reference by the method it denotes and, for a class-typed receiver, the receiver's identity hash (two static or extension references to one method, or two references to one method on one receiver, hash alike); a reference whose receiver is value-shaped by the identity of its own function object, as it compares. §8.3's invariant holds everywhere again; the hash stays as opaque as every identity hash. The function type stays not hashable (0161's option B2 is kept): a lambda equals only itself, so a hash container keyed on function values would hold every lambda evaluation apart — the reason 0161 gave that still stands. Measured reach: zero sites change verdict; the runtime gains one kind-dispatched hash beside its kind-dispatched equality (`bz_closure_value_equals`).

---

## 1. The problem

§27.7.1 (v0.334): "A function value's `GetHashCode` is `Object`'s identity hash (§8.3), assigned per function object, which need not agree with the equality above, so a function type is not **hashable** (§3.6, clause (e))". §3.6 clause (e): "a function type, whose `GetHashCode` is `Object`'s identity hash and need not agree with its equality". §8.3: "`GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`" — the table above it lists every type kind's default and names no exception. §28.3 *Set-erasing positions*: a capture-free function value "is admitted" at a position declared `Object`.

```bezel
static class Demo
  public static method Twice(n: int): int => n * 2;
  public static method Agree<T>(a: T, b: T): bool where T: hashable, notnull =>
    not (a = b) or a.GetHashCode() = b.GetHashCode();   // what `hashable` promises (§8.3)
  public static method Run()
    var fn1: (int) -> int := Demo.Twice;
    var fn2: (int) -> int := Demo.Twice;   // equal to fn1 (§27.7.1: a static reference to M equals every other)
    var f: Object := fn1;                  // capture-free: admitted at Object (§28.3)
    var g: Object := fn2;
    var ok := Agree<Object>(f, g);         // false whenever the identity hashes differ
    using var s := new Set<Object>();
    s.Add(f); s.Add(g);                    // Count is 1 or 2, by bucket placement
  end Run;
end Demo;
```

0161 §1.2 showed only `Set<(int) -> int>`, and its decline of option B1 read: "It defines a hash for a key nobody uses (`Set<(…) ->` and `Dictionary<(…) ->` have zero sites in every corpus) and a lambda's hash would still be identity, so the row would make a set of lambdas legal and useless; §27.7.1 already advised against the use". The first ground is false at `Object`, which keys a function value in every `Set<Object>` and at every `where T: hashable` instantiation at `Object`; the second and third grounds concern the function *type* as a key and are untouched here.

**What the compiler does today** (source-read at `fd8a9a2`; no build was run): equality is kind-dispatched — `bz_closure_value_equals` (`src/runtime/bezel_runtime_events.cpp:175`) compares by `{fn, receiver}` for a class-receiver reference, by `fn` for a static reference and by identity for a lambda or value-shaped receiver, the kind tagged at the cell by `initClosureKind` (`src/codegen/LlvmBackend_Layout.cpp:6737`, `7389`); the hash of a closure cell reached through `Object`'s `GetHashCode` slot (`LlvmBackend_Layout.cpp:6214`) is `bz_obj_identity_hash` of the cell (`src/runtime/bezel_runtime_refcounting.cpp:244`). The implementation is exactly the v0.334 text; the rule below is spec-ahead-of-impl.

---

## 2. Options

| | Option | `Set<Object>` of two `Demo.Twice` | `Agree<Object>(f, g)` | `Set<(int) -> int>` | Text |
|---|---|---|---|---|---|
| **S** | Status quo: identity hash, "need not agree" | 1 or 2 | may be `false` | rejected | §8.3's "must" has an unstated exception |
| **R1** | **Define the hash on the equality's key; keep the type not hashable — adopted** (#5792 reading 1) | 1 | `true` | rejected | one §27.7.1 sentence, one §8.3 row, clause (e) reworded |
| **R1′** | R1 and make the function type hashable again (0161's option B1 proper) | 1 | `true` | admitted, and a set of lambdas is legal and holds every evaluation apart | reopens 0161 B2 against its surviving grounds |
| **R2** | An identity `Equals` on the `Object` surface (#5792 reading 2) | 2 | `true` (both identity) | rejected | splits `=` from `Equals`, which §8.4 identifies |
| **R3** | Record the disagreement in §8.3 as a documented exception for function values (#5792 reading 3) | 1 or 2 | may be `false` | rejected | one parenthetical; `where T: hashable` at `Object` promises what a legal key breaks |

**R1 over R3.** R3 is the smallest text and leaves the correctness defect: a generic body relying on §8.3 at `Object` stays wrong, and §3.6's hashable definition ("types usable where consistent `Equals`/`GetHashCode` are required") stays false for `Object`. **R1 over R2.** §8.4: "`a = b` invokes `a.Equals(b)`"; an `Object`-surface `Equals` that differs from `=` would be the first such split in the language, and event unsubscription (`-=` matches by §27.7.1's equality) would then disagree with `Set<Object>.Remove`. **R1 over R1′.** The type's exclusion rests on a lambda's identity equality, which no hash definition changes; 0161's row keeps its bar (a measured program keying a hash container on method references), and nothing here meets it. **R1's cost.** One kind-dispatched hash in the runtime, mirroring the equality that already exists; a static reference's hash must not expose the method's address (§8.3: identity hashes "never expose addresses"), so it is drawn from the identity-hash sequence per method, as a class object's is.

---

## 3. The rule

▲ **§27.7.1.** "A function value's `GetHashCode` **agrees with this equality**, as §8.3 requires of every type, by being derived from exactly what the equality compares: a lambda's hash is the identity hash of its closure object (§8.3's assigned hash, never the receiver's or a capture's `GetHashCode`); a method reference's hash is derived from the method it denotes and, where the equality compares a receiver, from that receiver's identity hash, so any two references the equality makes equal hash alike; and a method reference whose receiver is value-shaped hashes by the identity of its own function object, as it compares. The hash is as opaque as every identity hash — a per-run value exposing no address — and holds at `Object`, where a capture-free function value is admitted (§28.3's *Set-erasing positions*) and is an ordinary key: a `Set<Object>` holds two evaluations of one static method's reference as one element, and `where T: hashable` at `Object` promises nothing a function value breaks. A function type is nonetheless not **hashable** (§3.6, clause (e)), because a lambda equals only itself, so a hash container keyed on function values would hold every lambda evaluation apart — the trap the `+=`/`-=` paragraphs below describe: a `Set<(int) -> int>`, …" (the rest of the sentence unchanged).

▲ **§3.6 clause (e).** "a function type (§27.7.1: a lambda equals only itself, so a hash container of function values would hold every lambda evaluation apart; a function value's hash agrees with that equality, and at `Object` it is an ordinary key)".

▲ **§8.3's table**, a new row after the unit row: *Function value (§27.7.1)* — default `Equals`: a lambda equals itself only; a method reference equals one denoting the same method on the same class-typed receiver, or the same static method; one over a value-shaped receiver equals itself only — default `GetHashCode`: a lambda, or a reference over a value-shaped receiver, its function object's identity hash; any other method reference, derived from the method and, where the equality compares a receiver, the receiver's identity hash.

▲ **Appendix C.1.** "nor is a function type, whose `GetHashCode` agrees with its equality but whose lambdas equal themselves only (§3.6 clause (e), §27.7.1)."

---

## 4. Soundness and consistency

Memory safety was never at stake: stdlib §4's *Stability while stored* keeps a hash container "memory-safe" under any hash. The rule restores §8.3's invariant as a universal — the property §3.6's hashable row, §9.2's *Through a type parameter* and every hash container's contract read — with no second reading. It composes with §27.7.1's equality by construction (the hash is a function of the equality's key: `(method, receiver identity)` for a reference, closure identity for a lambda), with §8.3's identity-hash assignment (every component is an assigned identity hash or a per-method value drawn the same way, so determinism under §30.13 and address-independence are kept), and with §28.3 (nothing changes which function values reach `Object`). The function type's exclusion keeps 0161's clause (e) and every rejection it lists (`Set<(int) -> int>`, `Dictionary<(int) -> int, V>`, stdlib §5's hashable block, `where T: hashable` at a function type); only the clause's ground changes, from "need not agree" to "a lambda equals only itself". Event unsubscription is untouched: `EventSource<T>.Remove` and `-=` match by the equality, not by hashing.

**Relation to recorded declines.** `DECLINED.md`'s 0161 option-B1 row declined "a function-type row in §8.3 defining `GetHashCode` on §27.7.1's (method, receiver) key, so that function types stay hashable". This proposal adopts the first half on new evidence against the row's first ground (the key is used at `Object`) and keeps the second half declined; the row is annotated, not struck, and its bar stands. No other row or rationale decline is touched (searched: "identity hash", "hashable", "GetHashCode").

---

## 5. Normative edits

The exact `old → new` set is `scratch/v0335/X3.json` (ids `X3-0173-*`): §27.7.1 (one sentence), §3.6 (clause (e)), §8.3 (one table row), Appendix C.1 (one clause), rationale §7.7 (one dated paragraph), the code-generation reference §7.2 (the `Set<(int) -> int>` comment), `DECLINED.md` (two rows and one annotation), `README.md` (this proposal's row).

---

## 6. Declined alternatives

| Alternative | Why declined | Reopening bar |
|---|---|---|
| An identity `Equals` on a function value's `Object` surface, so that hash and equality agree by making both identity (#5792 reading 2) | Splits `=` from `Equals`, which §8.4 identifies ("`a = b` invokes `a.Equals(b)`"); `-=` would then unsubscribe a re-evaluated method reference that `Set<Object>.Remove` could not find | — (none stated) |
| Recording the disagreement in §8.3 as a documented exception for function values held at `Object` (#5792 reading 3) | Leaves `where T: hashable` at `Object` promising what a legal key breaks, and §3.6's hashable definition false for `Object`; one parenthetical cannot carry an exception that every hash container's contract reads | — (none stated) |

0161's option B1 as stated — making the function type hashable again — stays declined (its row is annotated); its bar, a measured program keying a hash container on method references, is unchanged.

---

## 7. Migration and reach

Measured at `fd8a9a2` over `tests/` and `docs/`:

- **Newly rejected:** nothing. The rule changes no verdict: every rejection 0161 lists stands, and no admission changes.
- **Newly defined:** the hash of a function value — observable only through `GetHashCode()` on a function value or an `Object` holding one, and through hash-container placement of such values. No test, lit file, corpus case or document stores a function value in `Set<Object>`, `Dictionary<Object, V>`, `OwningSet<Object>` or `Counter<Object>` (`grep` for the four at `Object` finds `set_object_element_4642`, `dict_object_bare_value_key_indexer_set_4640`, `dict_class_key_2773`, `owningset_object_admitted_4657`, all over ints, strings and classes), and no test calls `GetHashCode()` on a function value; `hashable_constraint_2087`'s `where T: hashable` instantiations are at records and classes.
- **Doc-only:** Appendix C.1's clause, the §8.3 row, clause (e)'s ground.
- **Corpus:** no rule case states the identity-hash sentence or clause (e)'s ground (`manifest.tsv` searched for "27.7", "hashable", "identity hash"); no banked verdict changes. Coverage gap to write after landing: the `Set<Object>` program above with `Count = 1` (run), and `Agree<Object>(f, g)` printing `True`.
- **Guides:** none mentions a function value's hash; no pin moves.

---

## 8. Spec-ahead-of-impl

One `spec-tracking` issue at landing: the `GetHashCode` slot reached with a closure cell (`LlvmBackend_Layout.cpp`'s `Object`-member diamond, and every hash container's key path) dispatches on the cell's `FnValKind` as `bz_closure_value_equals` does — `BZ_FVK_REF_IDENTITY`: `bz_obj_identity_hash(cell)`; `BZ_FVK_FN_ONLY`: a per-method value drawn from the identity-hash sequence on first request (keyed by the `fn` pointer in a side table, never the address itself); `BZ_FVK_FN_PLUS_RECV`: that value combined order-sensitively with the receiver's identity hash. Regression rows: the §1 program (`Count = 1`, `Agree<Object>` true); two lambdas (`Count = 2`).

---

## 9. Composition

- Proposal 0172 (this lane) and the #5835 ruling touch §28.3, §28.14, §12.7, §9.7 and §12.9; no shared anchor with this proposal.
- X4's NR11 (R2/C5's interface face) decides the same cell on a different face; both notes should land in the matrix record.
- `README.md` and `DECLINED.md` rows are inserted at the live-queue tail and the `declined-at-proposal` group's end by sequential insertion with the other lanes (`scratch/v0334/comp_work/SEAMS.md`); the DECLINED index-size footer is recounted by the coordinator.

---

## 10. Questions

**Critic (v0.335).** The hash sentence was first drafted as "for a class-typed receiver … two static or extension references to one method hash alike", which copies §27.7.1's equality bullet ("A static (or extension-method) reference to `M` equals every other reference to `M`"). That bullet is imprecise for a *bound* extension reference over a class-typed receiver, which `closureKindForMethod` (`src/codegen/LlvmBackend_Layout.cpp:6661`) tags `FN_PLUS_RECV` and compares by receiver; the hash sentence now derives the hash from whatever the equality compares, so it is right under either reading, and the equality bullet's sharpening is recommended for filing (one `spec-question`: does `obj.Ext` over a class-typed `obj` equal `other.Ext`?). Also for the tracker: the per-method value for `FN_ONLY` is drawn once per process (a side table shared across domains, so two domains' references to one static method hash alike), from the requesting domain's §8.3 sequence, which keeps §30.13 determinism.

None filed by the lane. Considered and settled here: a property reference hashes as a method reference (the getter is the method, §27.7.1's fourth bullet); a bound extension-method reference over a class-typed receiver hashes by method and receiver, over a value-shaped receiver by its function object (as `closureKindForMethod` already classifies it for equality).
