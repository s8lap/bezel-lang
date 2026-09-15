// fix #4345 (codegen, §5.1 / §11.2 / §15.4 / §22.10, v0.298): a user-defined
// indexer setter's RHS (`h[i] := [...]`, `property [i: int]: Array<T>`) never
// actually materialized a collection literal — `lowerExpr` returns null for a
// `CollectionLiteralExpr` (§15.4, not a general expression value), the
// indexer-setter arm had no case for one at all, and the write silently
// degraded via the pre-existing "internal: indexer-setter RHS expression
// failed to lower (degraded — target retains its previous value) — #3025"
// backstop. MEASURED on origin/main = 9649e92 with the issue's own repro:
// `h[0] := [new Widget3Ok(s)]` printed the #3025 warning then crashed at
// runtime with `Unhandled exception: System.IndexOutOfRangeException`
// (`_items` stayed the constructor-time empty array; the setter never
// wrote). The sema half of this gap — the per-element conformance check —
// is `fix #4316`'s: the non-conforming twin of this program is correctly
// REJECTED at compile time with the same "not implicitly convertible" §5.1
// diagnostic #4310/#4316 already produce. THIS FIX is the codegen half of
// that pair: a literal RHS at an indexer-setter is now materialized the
// same way every other #4310-family position (local-decl / field-init /
// return-stmt) already does, and the resulting value reaches the setter
// body as a real owned heap collection (or, for an inline-array indexer,
// as a built `[N x elem]` aggregate). Target type is read from
// `ctx_.exprType(*as->target)` — the indexer element type — not the
// literal's own `exprType`, which (as #4316's filing noted) is never set
// at any #4310-family position.
//
// Scoped exactly to `lowerStmt`'s indexer-setter arm in
// `src/codegen/LlvmBackend_Stmts.cpp`. The `lowerExpr`-then-null arm
// immediately below this site is unchanged, so the #3025 backstop still
// fires for genuinely failed lowerings (e.g. an unhandled `in` form) —
// only the literal-handling gap is closed here, and it is closed BEFORE
// `lowerExpr`, exactly mirroring the return-stmt and field-init patterns.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

// Same diagnostic text the sibling #4310/#4316/#4323/#4330/#4340 suites
// assert — this fix is the codegen half of that pair, so the reject rows
// here pin the SAME gate on the indexer-setter position (not a hand-rolled
// per-element walk; LANE_RULES §6/§25).
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Note on diagnostic-absence checks: any "is this internal-warning absent?"
// assertion against `r.diagnostics` after `EXPECT_TRUE(r.ran)` is
// TAUTOLOGICAL — `runAot`'s `r.diagnostics` is empty by construction when
// the program ran (the harness reports Warnings through stderr, not via
// the returned `JitResult`; see scripts/check_vacuous_tests.py #3138).
// The actual oracle on the accept side is the runtime exit code.

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The Holder2 / Widget3Ok / C shape from the issue body — verbatim, since
// the issue itself is a pin of the exact repro. Kept as a string constant
// so a `git blame` on this test shows the exact source line every row
// exercises, and so a future regression that drifts the repro shape
// diffs visibly.
const char* kIssueRepro =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; "
    "class Holder2 "
    "  private _items: Array<IHolder>; "
    "  public constructor() _items := []; end constructor; "
    "  public property [i: int]: Array<IHolder> "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder2; "
    "class C "
    "  public static method Main(): int "
    "    var s := \"he\" + \"llo\"; "
    "    var h := new Holder2(); "
    "    h[0] := [new Widget3Ok(s)]; "
    "    return h[0][0].GetV().Length; "
    "  end Main; "
    "end C;";

// #4316's non-conforming twin (the indexer-setter row of the #4316 suite,
// same shape so this fix cannot be cherry-picked WITHOUT the #4316 gate
// and still pass — they are a pair).
const char* kNonConformingRepro =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget3 "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3; "
    "class Holder2 "
    "  private _items: Array<IHolder>; "
    "  public constructor() _items := []; end constructor; "
    "  public property [i: int]: Array<IHolder> "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder2; "
    "class C "
    "  public static method Main(): int "
    "    var s := \"he\" + \"llo\"; "
    "    var h := new Holder2(); "
    "    h[0] := [new Widget3(s)]; "
    "    return h[0][0].GetV().Length; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// PRIMARY ACCEPT-SIDE ROW (the bug as filed).
// ===========================================================================

// §5.1/§11.2/§15.4/§22.10 — the issue's own repro, verbatim. Pre-fix: the
// `#3025` backstop warning printed and the runtime exited 70
// (`IndexOutOfRangeException`). Post-fix: the literal is materialized as an
// `Array<IHolder>` heap collection, the setter writes it into `_items`,
// the read returns it intact, and `GetV().Length` is 5 (`"hello"`).
//
// Note on diagnostic-absence checks (scripts/check_vacuous_tests.py #3138):
// `runAot`'s `r.diagnostics` is empty by construction when `r.ran` is true
// (the harness reports Warnings through stderr, not via the returned
// `JitResult`), so an `EXPECT_FALSE(hasWarning(r))` is a TAUTOLOGY after
// `EXPECT_TRUE(r.ran)`. The oracle here is therefore the exit code — if
// the literal fails to materialize, the runtime takes the wrong path and
// exits non-5. The companion lit-file diagnostic oracle, if ever needed,
// would live at `tests/lit/indexer_setter_*_no_*_note_*.bzl`.
BEZEL_TEST(indexer_setter_collection_literal_materializes_4345,
           conforming_element_heap_array_writes_5_1_11_2_15_4_22_10) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// ACCEPT-SIDE CONTROLS — every one of these exercises a DIFFERENT position
// the indexer-setter lowering must continue to admit. A false-positive
// reject here would be worse than the bug this fix closes.
// ===========================================================================

// Plain-expression RHS on the same indexer shape — must stay accepted. A
// non-literal `value` was never affected by the gap (lowerExpr returns a
// real value for it), so the post-fix compiler must keep it working
// end-to-end. Uses the same `Array<int> := [...]` literal-bind pattern
// `fix #2725` accepts (the `new Array<int>([...])` constructor call was
// never valid syntax — that arm of the constructor list takes an optional
// `int` capacity, not a literal argument).
BEZEL_TEST(indexer_setter_collection_literal_materializes_4345,
           plain_value_rhs_unaffected_22_10) {
    auto r = runAot(
        "module M; "
        "class Holder2 "
        "  private _items: Array<int>; "
        "  public constructor() _items := []; end constructor; "
        "  public property [i: int]: Array<int> "
        "    get => _items; "
        "    set => _items := value; "
        "  end; "
        "end Holder2; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder2(); "
        "    var src: Array<int> := [10, 20, 30]; "
        "    h[0] := src; "
        "    return h[0][0] + h[0][1] + h[0][2]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// INLINE-array indexer setter with a literal RHS — the OTHER materialization
// arm of #4345's fix (the inline-array aggregate build, distinct from the
// heap-collection path above). Pre-fix: same #3025 degrade. Post-fix: the
// literal is built as `[N x elem]` by-value, the setter writes the
// aggregate into `_items`, and the read returns it intact. Mirrors the
// inline-array twin of the heap-array accept row above, exercising the
// `kind == InlineArray` branch of the new arm. Bound to a named local
// rather than chained (`h[0][0] + h[0][1]`) because the chained form is
// a pre-existing INDEPENDENT codegen gap (#2491 backstop on a by-value
// inline-array temporary indexer-get) — out of scope for this fix and
// unchanged by it; the named-local form exercises the SETTER path
// directly while still pinning the materialized literal's stored value.
BEZEL_TEST(indexer_setter_collection_literal_materializes_4345,
           inline_array_indexer_literal_writes_5_1_15_1_22_10) {
    auto r = runAot(
        "module M; "
        "class Holder3 "
        "  private _items: [2]int; "
        "  public property [i: int]: [2]int "
        "    get => _items; "
        "    set => _items := value; "
        "  end; "
        "end Holder3; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder3(); "
        "    h[0] := [10, 20]; "
        "    var got: [2]int := h[0]; "
        "    return got[0] + got[1]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

// Heap-collection NON-indexer assignment (`outer := [...]` to a plain
// `Array<T>` local) — unaffected by this fix (#736 already covers it),
// but included as a sibling-position control so a regression in either
// materialization path is diffed against the other.
BEZEL_TEST(indexer_setter_collection_literal_materializes_4345,
           plain_local_heap_array_literal_unaffected_15_4) {
    auto r = runAot(
        "module M; "
        "interface IHolder "
        "  method GetV(): string; "
        "end IHolder; "
        "class Widget3Ok: IHolder "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "  public method GetV(): string return V; end GetV; "
        "end Widget3Ok; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer: Array<IHolder>; "
        "    outer := [new Widget3Ok(s)]; "
        "    return outer[0].GetV().Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Pre-existing user-indexer plain-assignment case (a non-literal RHS —
// proves the new arm does not silently capture a non-literal path and
// accidentally regress plain-assignment correctness). The pre-fix
// `lowerExpr`-then-null path is the one that handles this shape; the new
// arm declines it (it is gated on a `CollectionLiteralExpr`), so the
// existing handling stays in place. The oracle is the exit code (42).
BEZEL_TEST(indexer_setter_collection_literal_materializes_4345,
           non_literal_user_indexer_value_unaffected_22_10) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private _v: int; "
        "  public constructor() _v := 0; end constructor; "
        "  public property [i: int]: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h[0] := 42; "
        "    return h[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// REJECT ROW — sema-level (#4316) gate at the indexer-setter position.
// This row is the cross-fix anchor: it MUST pass before AND after this
// fix lands, because the codegen materialization is only engaged AFTER
// the sema gate has admitted the program. A future regression in either
// direction (a non-conforming program silently accepted, or a
// conforming program spuriously rejected) fails here.
// ===========================================================================

// #4316's `indexer_setter_noconform_rejected` row, copied verbatim —
// the codegen fix is a NO-OP for this row (the sema side rejects before
// codegen sees the literal), so it must keep passing.
BEZEL_TEST(indexer_setter_collection_literal_materializes_4345,
           non_conforming_element_rejected_5_1_11_2) {
    auto r = runAot(kNonConformingRepro);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
