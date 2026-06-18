// fix #787 / §9.4 / §14.1: a generic record instantiated with a BUILTIN
// COLLECTION type argument (`Box<Array<int>>`, `Box<Dictionary<K,V>>`) lays out
// and constructs the field correctly.
//
// The record-construction and local-slot paths each have an `allUnbound` guard
// that skips the `{T → concrete}` substitution when every generic argument is an
// unbound type parameter (so the template body stays unspecialized). A builtin
// collection like `Array<int>` is — like a bare type parameter — a Named,
// declaration-less, single-part name ("Array") not in the active substitution,
// so it was MISCLASSIFIED as unbound. `new Box<Array<int>>` then built the
// UNSPECIALIZED `{i32}` struct, truncating the 8-byte Array pointer into a
// 4-byte field, while field access used the specialized `{ptr}` view → a layout
// mismatch and a garbage pointer → SIGSEGV. The fix adds `!arg->genericArgs
// .empty()` to both mirrored guards: a type carrying its own generic arguments
// is concrete, not an unbound parameter. (A bare `T` has no generic args, so the
// generic-method template path is unaffected.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §14.1: the #787 repro — a method on the Array<int> field. Was SIGSEGV; now 2.
BEZEL_TEST(generic_record_collection_arg_787, array_field_method) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T); "
        "class C public static method Main(): int "
        "  using var a: Array<int> := new Array<int>(2); a.Add(1); a.Add(2); "
        "  var b := new Box<Array<int>>(a); "
        "  return b.V.Count(); "  // 2
        "end Main; end C;"), 2);
}

// §14.1: the field's elements read back correctly (not just non-crash) — Sum.
BEZEL_TEST(generic_record_collection_arg_787, array_field_values) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T); "
        "class C public static method Main(): int "
        "  using var a: Array<int> := new Array<int>(3); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var b := new Box<Array<int>>(a); "
        "  return b.V.Sum(); "  // 60
        "end Main; end C;"), 60);
}

// §14.1: a nested-generic collection arg with two type params (Dictionary<K,V>).
BEZEL_TEST(generic_record_collection_arg_787, dictionary_field) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T); "
        "class C public static method Main(): int "
        "  using var d: Dictionary<string,int> := new Dictionary<string,int>(); "
        "  d.Add(\"k\", 7); "
        "  var b := new Box<Dictionary<string,int>>(d); "
        "  return b.V[\"k\"]; "  // 7
        "end Main; end C;"), 7);
}

// §14.1 control: a reference-type (string) arg still works (it already
// specialized — string is Primitive-kind, never misclassified as unbound).
BEZEL_TEST(generic_record_collection_arg_787, string_arg_unchanged) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T); "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hi\"); "
        "  return b.V.Length; "  // 2
        "end Main; end C;"), 2);
}

// §14.1 control: a generic-method template `new Box<T>` (T unbound) must stay
// unspecialized — the fix only excludes args that carry their own generic args.
BEZEL_TEST(generic_record_collection_arg_787, generic_method_template_unchanged) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T); "
        "static class F "
        "  public static method Make<T>(v: T): Box<T> return new Box<T>(v); end Make; "
        "end F; "
        "class C public static method Main(): int "
        "  var b := F.Make<int>(9); return b.V; "  // 9
        "end Main; end C;"), 9);
}
