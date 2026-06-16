// Spec-derived regression tests for fix #747 (R42 finding #0) — accessing a
// STATIC field through an INSTANCE receiver (`c._x` where `_x` is `public
// static`) crashed the COMPILER with a SIGSEGV during codegen.
//
// Authority: §22.12 (static state). Root cause: a static field has no slot in
// the instance struct, so its FieldSymbol is absent from `fieldIndex_`.
// `Emitter::fieldAddr` looked the symbol up, found `end()`, and then
// dereferenced that end iterator (`idxIt->second`) to index the struct layout —
// running off the end and segfaulting the compiler. The fix returns `nullptr`
// from `fieldAddr` when the member is not in the instance layout, so the read
// path (LlvmBackend_Exprs.cpp) and the write path (lvalueAddr) both fall through
// to `staticFieldGlobal()` — the same program-lifetime backing global a
// type-qualified `C._x` access already reads. Instance and type access therefore
// reach the SAME static storage.
//
// NOTE (open design question, flagged on #747): the spec frames static-member
// access as being on the type name in receiver position (§22.12); whether an
// instance-receiver static access like `c._x` should instead be REJECTED with a
// sema diagnostic (cf. C# CS0176) is a separate design call. These tests pin the
// crash-freedom + shared-storage semantics that hold *while* the access is
// accepted; if it is later made a diagnostic, they move to the sema suite.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §22.12: the headline — reading a static field through an instance receiver
// must not crash the compiler and must read the static value (7).
BEZEL_TEST(static_field_via_instance_747, read_through_instance) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _x: int := 7; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c._x; "  // instance-receiver read of a static field
        "  end Main; "
        "end C;"), 7);
}

// §22.12: a write through an instance receiver reaches the same static storage,
// observable through a type-qualified read (`C._x`).
BEZEL_TEST(static_field_via_instance_747, write_through_instance_shared_storage) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _x: int := 7; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c._x := 9; "        // instance-receiver write...
        "    return C._x; "      // ...observed through the type → 9
        "  end Main; "
        "end C;"), 9);
}

// §22.12: the converse — a type-qualified write is observed through an
// instance-receiver read; both name the one backing global.
BEZEL_TEST(static_field_via_instance_747, type_write_instance_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _x: int := 0; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    C._x := 42; "       // type-qualified write...
        "    return c._x; "      // ...observed through the instance → 42
        "  end Main; "
        "end C;"), 42);
}

// §22.12: a non-int static field (string length) read through an instance —
// the layout-miss guard is type-agnostic, so a wider/other-typed static field
// is equally safe.
BEZEL_TEST(static_field_via_instance_747, read_through_instance_long) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: long := 5000000000L; "  // > 2^32, needs i64 storage
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return int(c._n - 4999999993L); "  // 5000000000 - 4999999993 = 7
        "  end Main; "
        "end C;"), 7);
}
