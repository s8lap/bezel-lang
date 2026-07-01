// Regression tests for #1107 / stdlib §2.17 / §31.4 (sweep #9, findings #11/#12/
// #13): `TypeInitializationException.InnerException`. When a static initializer
// throws, the runtime wraps the original exception in a TypeInitializationException
// and stores it at byte offset 16 of the 24-byte object. But `ex.InnerException`
// had no property-getter lowering — it fell to a generic getter that returned
// null, so `inner is not null` (#11) and `inner is ArgumentException` (#12) were
// false, and binding the result to a local then testing `is`/`as` dereferenced
// null (#13 SIGSEGV). The getter now reads offset 16 (the inner exception ptr,
// nullable `Exception?`). Found by adversarial discovery sweep #9.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kFailingInit =
    "class FS "
    "  public static _v: int := Throw(); "
    "  private static method Throw(): int "
    "    throw new ArgumentException(\"inner-msg\"); return 0; "
    "  end Throw; "
    "end FS; ";
}  // namespace

// #1107/#11: `ex.InnerException is not null` is true (the wrapped original is
// present) — was false.
BEZEL_TEST(type_init_inner_exception_1107, inner_is_not_null) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kFailingInit +
        "class C public static method Main(): int "
        "  try var _ := FS._v; "
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException is not null then return 55; else return 1; end; "
        "  end end; "
        "  return 0; "
        "end Main; end C;"), 55);
}

// #1107/#12: `ex.InnerException is ArgumentException` is true (the stored inner
// is the ArgumentException the initializer threw) — was false.
BEZEL_TEST(type_init_inner_exception_1107, inner_is_derived_type) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kFailingInit +
        "class C public static method Main(): int "
        "  try var _ := FS._v; "
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException is ArgumentException then return 55; else return 1; end; "
        "  end end; "
        "  return 0; "
        "end Main; end C;"), 55);
}

// #1107/#13: binding the InnerException to a local then testing `is` no longer
// crashes (was a SIGSEGV on the null deref).
BEZEL_TEST(type_init_inner_exception_1107, bind_then_is_no_crash) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kFailingInit +
        "class C public static method Main(): int "
        "  try var x: int := FS._v; "
        "  catch var ex: TypeInitializationException => do "
        "    var inner: Exception? := ex.InnerException; "
        "    if inner is ArgumentException then return 55; end; "
        "  end end; "
        "  return 0; "
        "end Main; end C;"), 55);
}

// The inner exception is a fully-valid object: it can be `as`-cast and its
// `.Message` read.
BEZEL_TEST(type_init_inner_exception_1107, inner_as_cast_and_message) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kFailingInit +
        "class C public static method Main(): int "
        "  try var _ := FS._v; "
        "  catch var ex: TypeInitializationException => do "
        "    var inner: Exception? := ex.InnerException; "
        "    if inner <> null then "
        "      var ae := inner! as ArgumentException; "  // #1266-sibling: local-bind path
        "      if ae <> null then return ae!.Message.Length; end; "  // "inner-msg" = 9
        "    end; "
        "  end end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

// #1266: the DIRECT chained form `ex.InnerException!.Message` (no explicitly-typed
// local bind) must also read the inner exception's message. sema does not type
// `ex.InnerException` (the builtin TypeInitializationException has no PropertyDecl),
// so the codegen Message handler recognises the InnerException chain structurally.
// Before the fix this returned 0 (empty), while the local-bind form above worked.
// "inner-msg" = 9.
BEZEL_TEST(type_init_inner_exception_1107, inner_message_direct_chain_1266) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kFailingInit +
        "class C public static method Main(): int "
        "  try var _ := FS._v; "
        "  catch var ex: TypeInitializationException => do "
        "    return ex.InnerException!.Message.Length; "  // 9 (was 0)
        "  end end; "
        "  return -1; "
        "end Main; end C;"), 9);
}
