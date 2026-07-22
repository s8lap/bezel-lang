// Codegen (AOT) — §7.4 readonly-field flow narrowing (v0.214, proposal 0058,
// issue #2085).
//
// §7.4 (v0.214): a nullable `readonly` field — instance or `static` —
// participates in flow narrowing, because the blanket refusal's premise (unseen
// writers) does not exist for it: outside its construction window a `readonly`
// field has no writer on any path. These programs must COMPILE (sema accepts the
// narrowed field access) AND produce the correct value at runtime — the narrowed
// read must load the field's payload, not misread it. Observable via the int
// exit code; a wrong value would flag either a sema mis-narrowing or a codegen
// mislowering.
//
// The reject side (mutable field still errors; `a.b._f` establishes nothing;
// ctor-window unnarrowed; root-reassign / captured-root invalidation) lives in
// the lit/FileCheck track (tests/lit/sema/readonly_field_narrowing_2085__*).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §7.4 (v0.214): a nullable readonly INSTANCE field narrows off `this` (bare
// `_name`). "hello".Length == 5 flows through the narrowed then-block.
BEZEL_TEST(codegen_readonly_field_narrow_2085, this_instance_reads_payload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _name <> null then return _name.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(\"hello\"); return c.Len(); "
        "  end Main; end C;"), 5);
}

// §7.4 (v0.214): the null referent takes the fall-through path (the guard is
// false), so no narrowed read occurs and the method returns -1 (0xFF & -1 ...):
// use a positive sentinel to keep the 8-bit exit code unambiguous.
BEZEL_TEST(codegen_readonly_field_narrow_2085, this_instance_null_falls_through) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _name <> null then return _name.Length; end; return 9; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(null); return c.Len(); "
        "  end Main; end C;"), 9);
}

// §7.4 (v0.214): a `static readonly` nullable field narrows off its type name.
// "hi".Length == 2.
BEZEL_TEST(codegen_readonly_field_narrow_2085, static_field_reads_payload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Counter public static readonly _label: string? := \"hi\"; "
        "  public static method Main(): int "
        "    if Counter._label <> null then return Counter._label.Length; end; "
        "    return -1; "
        "  end Main; end Counter;"), 2);
}

// §7.4 (v0.214): a `this`-rooted readonly fact survives an intervening call (a
// readonly field has no writer). The Noise() call between the guard and the use
// does not invalidate; "hi".Length == 2.
BEZEL_TEST(codegen_readonly_field_narrow_2085, survives_intervening_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Noise() end Noise; "
        "  public method Len(): int "
        "    if _name <> null then Noise(); return _name.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(\"hi\"); return c.Len(); "
        "  end Main; end C;"), 2);
}

// §7.4 (v0.214): `local._f` narrows off a narrowing-tracked (borrowed, never
// reassigned) local root; the field read returns "world".Length == 5.
BEZEL_TEST(codegen_readonly_field_narrow_2085, local_root_reads_payload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Holder public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; end Holder; "
        "class C public static method Use(h: Holder): int "
        "    if h._name <> null then return h._name.Length; end; return -1; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var h := new Holder(\"world\"); return Use(h); "
        "  end Main; end C;"), 5);
}

// §7.4 (v0.214): a nullable readonly VALUE-type (record) field narrows and its
// stored field reads correctly through the narrowed path — Box(7).V == 7.
BEZEL_TEST(codegen_readonly_field_narrow_2085, value_type_field_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(V: int); "
        "class Holder public readonly _b: Box?; "
        "  public constructor(b: Box?) _b := b; end constructor; "
        "  destructor end destructor; "
        "  public method Val(): int "
        "    if _b <> null then return _b.V; end; return -1; "
        "  end Val; "
        "  public static method Main(): int "
        "    using var h := new Holder(new Box(7)); return h.Val(); "
        "  end Main; end Holder;"), 7);
}

// §7.4 (v0.214) composed with §22.7: a non-virtual helper reached during
// construction narrows `this._name` normally (the ctor-window exclusion is
// textual); post-construction the same helper reads "hey".Length == 3.
BEZEL_TEST(codegen_readonly_field_narrow_2085, this_escape_helper_narrows) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; Helper(); end constructor; "
        "  destructor end destructor; "
        "  public method Helper(): int "
        "    if _name <> null then return _name.Length; end; return 0; "
        "  end Helper; "
        "  public static method Main(): int "
        "    using var c := new C(\"hey\"); return c.Helper(); "
        "  end Main; end C;"), 3);
}
