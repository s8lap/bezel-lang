// fix #768 / §14.1 / §23: a generic type's property getter reads its (possibly
// nullable) fields with the receiver's CONCRETE type arguments.
//
// The CG33b property-getter path set up genericSubst_ (the LLVM type map) from
// the receiver's type args but NOT genericSubstSemaTy_ (the sema-type map). The
// getter body resolves a member type like `_v: T?` through genericSubstSemaTy_,
// so without it the field was read with the wrong (unresolved) type — a wrong
// value (and, before an earlier fix, an ICmp type-mismatch SIGABRT). The fix
// builds genericSubstSemaTy_ alongside genericSubst_ and restores both.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §14.1 / §23: a property on `Box<string>` returning the `T?` field reads back
// the non-null string. (The #768 repro: `b.Val` was wrongly null.)
BEZEL_TEST(generic_property_getter_768, class_typed_nullable_property) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<T> where T: class "
        "  private _v: T?; "
        "  public constructor(v: T) _v := v; end; "
        "  public property Val: T? => _v; "
        "end Box; "
        "public static class Program "
        "  public static method Main(): int "
        "    using var b := new Box<string>(\"hello\"); "
        "    if b.Val = null then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// §14.1 / §23: a VALUE-type `T?` property — `Box<int>`'s property returns the
// int? field, which unwraps to the stored value.
BEZEL_TEST(generic_property_getter_768, value_typed_nullable_property) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box<T>(V: T?) "
        "  public property Held: T? => V; "
        "end Box; "
        "class P public static method Main(): int "
        "  var b := new Box<int>(21); "
        "  if b.Held = null then return 1; end; "
        "  return b.Held!; "  // 21
        "end Main; end P;"), 21);
}

// §14.1 / §23: a getter that force-unwraps the `T?` field and returns it — the
// codegen field read resolves to the concrete type, so the value flows through.
//
// fix #4768 (§5.1 / §3.6 rule 2, v0.307): this row used to declare the getter
// `Once: int => V!` — a bare type-parameter value (`V!: T`) returned at an
// `int` target inside the generic body. §5.1 converts a type-parameter value
// only to `Object` and its constraint bounds, never to a primitive, so that
// spelling is now rejected at the declaration. The property is typed `T`
// instead: the SAME force-unwrap getter, and `Box<int>`'s `b.Once` is `int` at
// the call site, so the codegen path this row pins (the getter's field read
// under the receiver's concrete type args) is unchanged and still yields 21.
BEZEL_TEST(generic_property_getter_768, getter_force_unwraps_field) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box<T>(V: T?) "
        "  public property Once: T => V!; "
        "end Box; "
        "class P public static method Main(): int "
        "  var b := new Box<int>(21); "
        "  return b.Once; "  // 21
        "end Main; end P;"), 21);
}
// NOTE: a getter body that COMPUTES on the unwrapped field (e.g. `V! + V!`)
// currently fails sema — `V!` inside a generic property body is typed `?`
// (a separate sema field-type-resolution gap, NOT this codegen fix). Left out.
