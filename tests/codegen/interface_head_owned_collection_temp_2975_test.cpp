// Spec-derived regression test for issue #2975, manifestation 3.
//
// §28.3 (the `using var` scope-bound auto-drop) / §28.9 (release discipline) /
// §29.2 (desugaring of `for`, the IEnumerable coercion) / §28.2 (exactly once),
// v0.240. stdlib §4.4 (`Array<T>`), stdlib §4.1–§4.3 (the list interfaces).
//
// THE RULE. §28.3 requires the value a `using var` binds to be disposed at the
// end of its block. When that value is an unbound temporary — an allocation
// bound to no other name — this binding is its ONLY owner, so this scope end is
// its only disposal site.
//
// THE DEFECT. The §29.2 local-decl coercion widens an array-backed collection
// bound to a pure `IEnumerable<T>` local into a vtable-backed enumerator, so the
// for-in's iterator protocol is satisfied. It used the BORROWING wrapper
// unconditionally, whose slot-1 Dispose is `free(self)` — the wrapper frame
// only, never the backing. That is exactly right when the source has its own
// owner, and a LEAK when it does not.
//
// MEASURED on 618c0c8 (LeakSanitizer, 200 iterations of
// `using var e: IEnumerable<int> := new Array<int>(2);`):
//
//   Direct leak of 9600 byte(s) in 200 object(s)   ... in bz_array_alloc
//   Indirect leak of 1600 byte(s) in 200 object(s) ... in bz_array_alloc
//   SUMMARY: AddressSanitizer: 11200 byte(s) leaked in 400 allocations
//
// while the SAME loop at the `IList<int>` head — which takes no wrap at all and
// keeps the array-backed protocol — was clean on the same build. Only the head
// that takes the wrap leaked, which is what localised the defect to the wrap.
//
// THE FIX uses the OWNING enumerator for this shape (its slot-1 Dispose frees
// the backing AND the frame, exactly once) — the same treatment #2299 already
// gave the RETURN coercion for the identical shape. It is gated on the
// initializer being a `new` EXPRESSION rather than the wider "callee-owned
// producer" predicate the return site uses, because at a local-decl site a
// producer-init IDENTIFIER source is still owned by that identifier and taking
// ownership here would DOUBLE-FREE it. The alias cases below pin that.
//
// WHY THESE ORACLES. Under-release is LeakSanitizer's direction and
// over-release is AddressSanitizer's, so every case is a loop-amplified
// `runAotChecked` run, which is both. The alias cases additionally ITERATE and
// sum, so a source freed out from under the loop shows up as a wrong total
// rather than passing quietly.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// 200 iterations of `body`, then 0.
std::string loop(const std::string& body) {
    return "var i := 0; while i < 200 do " + body + " i := i + 1; end; "
           "return 0;";
}

}  // namespace

// ---------------------------------------------------------------------------
// The defect: an unbound `new Array<int>(2)` widened to the pure
// `IEnumerable<int>` head. 11200 bytes leaked on 618c0c8.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           array_temp_at_ienumerable_head_leak_free_28_3) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(loop(
            "using var e: IEnumerable<int> := new Array<int>(2);"))),
        0);
}

// The plain-`var` spelling of the same binding.
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           array_temp_at_ienumerable_head_plain_var_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(loop(
            "var e: IEnumerable<int> := new Array<int>(2);"))),
        0);
}

// A Stack source keeps the (borrowing) reverse-order wrap #2486 installed for
// iteration order, so it is deliberately NOT switched to the owning wrapper —
// its residual leak is recorded on #2975 rather than papered over here. The
// case pinned instead is the one the fix DOES cover with a non-`Array` head
// name in the same family: `OwningBuffer<T>` is bz_array_t-backed (stdlib
// §4.14) and takes the same wrap.
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           owning_buffer_temp_at_ienumerable_head_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(loop(
            "using var e: IEnumerable<int> := new OwningBuffer<int>(2);"))),
        0);
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once, the OVER-release direction — the reason the gate is the
// `new` EXPRESSION and not "a producer-init source". Here the array has a NAMED
// owner that disposes it at ITS scope end; the widened binding is an ALIAS and
// must take no ownership. If it did, the array would be freed twice, and the
// for-in below would read it after the free.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           named_array_alias_not_freed_twice_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; var tot := 0; "
            "while i < 200 do "
            "  using var a := new Array<int>(4); "
            "  a.Add(7); "
            "  var e: IEnumerable<int> := a; "
            "  for var v in e do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            "if tot <> 1400 then return 95; end; "
            "return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// CONTROLS: the richer heads take no wrap and keep the array-backed protocol.
// Each was clean on 618c0c8 and is what isolated the defect to the wrap; they
// must stay clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           array_temp_at_ilist_head_still_clean_4_1) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(loop(
            "using var e: IList<int> := new Array<int>(2);"))),
        0);
}

BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           array_temp_at_ireadonlylist_head_still_clean_4_1) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(loop(
            "using var e: IReadOnlyList<int> := new Array<int>(2);"))),
        0);
}

// The concrete-typed binding — the collection disposal path proper — is
// untouched.
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           concrete_array_using_var_unchanged_4_4) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(loop(
            "using var e: Array<int> := new Array<int>(2);"))),
        0);
}

// ---------------------------------------------------------------------------
// GUARD: the wrapped source must still be ITERABLE — the owning wrapper shares
// the plain machinery's MoveNext/Current, so a fix that freed the backing too
// early would show up as a wrong sum, not just as a sanitizer report.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_owned_coll_temp_2975,
           owned_wrap_still_iterates_its_elements_29_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var tot := 0; var i := 0; "
            "while i < 50 do "
            "  using var e: IEnumerable<int> := new Array<int>(4); "
            "  for var v in e do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            "if tot <> 0 then return 96; end; "
            "return 0;")),
        0);
}
