// Regression tests for #6317 (§9.4 records are value types / §35.1 choices are
// value types, spec v0.338): a record or choice instance method that STORES
// `this` into an Array element or a nullable property slot crashed. `this`
// lowers to the receiver's ADDRESS (CG29); `Array.Add(this)` spilled that
// pointer into a pointer-sized slot and `bz_array_add` copied the struct's
// byte size out of it (stack-buffer-overflow / garbage), and `r.P := this`
// into a `Rec?` slot stored the raw pointer into the tagged `{i8, T}`.
// The fix loads the struct value (coerceThisRecordValue) at both sites.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Array<Record>.Add(this) then read the field back.
BEZEL_TEST(record_this_stored_6317, record_add_this_to_array) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public record Tag "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public method Stash(a: Array<Tag>) a.Add(this); end Stash; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<Tag>(); "
        "    var t := new Tag(7); "
        "    t.Stash(a); "
        "    return a[0].N; "
        "  end Main; "
        "end Program;"), 7);
}

// `Array<Record?>.Add(this)` (nullable element).
BEZEL_TEST(record_this_stored_6317, record_add_this_to_nullable_array) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public record Tag "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public method Stash(a: Array<Tag?>) a.Add(this); end Stash; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<Tag?>(); "
        "    var t := new Tag(7); "
        "    t.Stash(a); "
        "    return a[0]!.N; "
        "  end Main; "
        "end Program;"), 7);
}

// `obj.P := this` into a `Rec?` property slot.
BEZEL_TEST(record_this_stored_6317, record_store_this_to_nullable_property) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public record Tag "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public method Put(r: Registry) r.LastTag := this; end Put; "
        "end Tag; "
        "public class Registry "
        "  public property LastTag: Tag?; "
        "end Registry; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new Registry(); "
        "    var t := new Tag(7); "
        "    t.Put(r); "
        "    return r.LastTag!.N; "
        "  end Main; "
        "end Program;"), 7);
}

// Choice instance method: Array<Choice>.Add(this).
BEZEL_TEST(record_this_stored_6317, choice_add_this_to_array) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public choice C "
        "  A(x: int), "
        "  B(y: int); "
        "  public method Stash(a: Array<C>) a.Add(this); end Stash; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<C>(); "
        "    var t: C := C.A(7); "
        "    t.Stash(a); "
        "    var r := 0; "
        "    case a[0] of A(x) => r := x, B(y) => r := y + 100; end case; "
        "    return r; "
        "  end Main; "
        "end Program;"), 7);
}
