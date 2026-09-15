// fix #4345 property-setter twin (codegen, §5.1 / §11.2 / §15.4, v0.298):
// a LOCAL computed-property setter's RHS (`h.P := [...]`, `property P: Array<T>`
// with `set => _f := value`) never actually materialized a collection literal —
// `lowerExpr` returns null for a `CollectionLiteralExpr` (§15.4, not a general
// expression value), and `lowerPropertySet` (LlvmBackend_Assign.cpp) had no
// case for one at all. The write silently degraded via the pre-existing
// "internal: assignment target lvalue address failed to resolve (store dropped —
// target retains its previous value) — #4045" backstop. MEASURED on the
// pre-fix compiler: `h.P := [new Widget3Ok(s)]` printed the #4045 warning
// then crashed at runtime with `IndexOutOfRangeException` (`_items` stayed
// the constructor-time empty Array; the setter never wrote).
//
// The sema half of this gap — the per-element conformance check — is
// `fix #4316`'s: the non-conforming twin of this program is correctly
// REJECTED at compile time with the same "not implicitly convertible" §5.1
// diagnostic #4310/#4316 already produce. THIS FIX is the codegen half of
// that pair for the property-setter position: a literal RHS at a property
// setter is now materialized the same way the indexer-setter fix
// (d408509) and every other #4310-family position (local-decl / field-init /
// return-stmt) already do. Target type is read from
// `ctx_.exprType(target)` — the property's declared type — the same trick
// every #4310-family position uses.
//
// The same gap existed in `lowerImportedPropertySet` (the cross-module
// `importedObj.P := [...]` path) and is closed here alongside it; the
// import case has no dedicated test row because both code paths share the
// same materialization helpers and the same `ctx_.exprType(target)` lookup.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The Heap-collection shape from the issue body — verbatim from the indexer
// setter test's kIssueRepro, adapted for the property setter spelling.
const char* kHeapCollectionRepro =
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
    "  public property P: Array<IHolder> "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder2; "
    "class C "
    "  public static method Main(): int "
    "    var s := \"he\" + \"llo\"; "
    "    var h := new Holder2(); "
    "    h.P := [new Widget3Ok(s)]; "
    "    return h.P[0].GetV().Length; "
    "  end Main; "
    "end C;";

// #4316's non-conforming twin: Widget3 does NOT implement IHolder, so the
// collection literal element is not implicitly convertible to Array<IHolder>.
// The #4316 sema gate rejects this at compile time before codegen runs.
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
    "  public property P: Array<IHolder> "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder2; "
    "class C "
    "  public static method Main(): int "
    "    var s := \"he\" + \"llo\"; "
    "    var h := new Holder2(); "
    "    h.P := [new Widget3(s)]; "
    "    return h.P[0].GetV().Length; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// PRIMARY ACCEPT-SIDE ROW (the bug as filed).
// ===========================================================================

// §5.1/§11.2/§15.4 — the issue's own repro adapted for a property setter.
// Pre-fix: the #4045 backstop warning printed and the runtime exited with
// IndexOutOfRangeException. Post-fix: the literal is materialized as an
// Array<IHolder> heap collection, the setter writes it into _items, the read
// returns it intact, and GetV().Length is 5 ("hello").
BEZEL_TEST(property_setter_collection_literal_materializes_4345,
           conforming_heap_array_writes_5_1_11_2_15_4) {
    auto r = runAot(kHeapCollectionRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// ACCEPT-SIDE CONTROLS
// ===========================================================================

// Inline-array property setter with collection literal — the INLINE-array twin
// of the primary row above. A property typed as [N]T receiving [e1, ..., eN]
// must build the [N x elem] aggregate by-value.
BEZEL_TEST(property_setter_collection_literal_materializes_4345,
           conforming_inline_array_writes_15_1_15_4) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private _items: [3]int; "
        "  public constructor() _items := [0, 0, 0]; end constructor; "
        "  public property P: [3]int "
        "    get => _items; "
        "    set => _items := value; "
        "  end; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h.P := [10, 20, 30]; "
        "    var got: [3]int := h.P; "
        "    return got[0] + got[1] + got[2]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// Plain-expression RHS on the same property shape — must stay accepted.
// A non-literal `value` was never affected by the gap (lowerExpr returns a
// real value for it), so the post-fix compiler must keep it working.
BEZEL_TEST(property_setter_collection_literal_materializes_4345,
           plain_value_rhs_unaffected_15_4) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private _items: Array<int>; "
        "  public constructor() _items := []; end constructor; "
        "  public property P: Array<int> "
        "    get => _items; "
        "    set => _items := value; "
        "  end; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    var arr := new Array<int>(); "
        "    arr.Add(42); "
        "    h.P := arr; "
        "    return h.P[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// REJECT-SIDE CONTROL (sema gate, mirrors the indexer-setter test's row)
// ===========================================================================

// §5.1/§11.2 — #4316's non-conforming twin. The sema gate (fix #4316) rejects
// the program at compile time before codegen runs; this row pins that the
// property-setter position is covered by the same gate.
BEZEL_TEST(property_setter_collection_literal_materializes_4345,
           non_conforming_element_rejected_5_1_11_2) {
    auto r = runAot(kNonConformingRepro);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}
