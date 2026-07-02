// Spec-derived tests for FFI-boundary type validation in `extern` method
// signatures (§32.5).
//
// §32.5 — "Only the following types may appear in `extern` method signatures":
//   - All integer types and `float`, `double`.
//   - `bool`.
//   - `IntPtr`, `UIntPtr`.
//   - `CString`, `WString`, `RawBytes`.
//   - Inline arrays `[N]T` where T is an FFI-compatible primitive.
//   - The unit type `()` — valid as a return type only.
//   - Function types `(...) -> T` whose params and return are FFI-compatible.
// §32.5 — "Not allowed: Bezel `string` ..., `char` ..., classes, records,
//   interfaces, generic type parameters, `Array<T>`, `OwningArray<T>`,
//   inline arrays whose element type is not FFI-compatible."
//
// The check runs during type resolution (Resolver::checkExternType). Each
// case asserts that the §32.5 diagnostic fired (or did not).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseResolve(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic is the §32.5 FFI-boundary-type message. Used so the
// "rejected" tests assert that *this* check fired (not an unrelated error),
// and the "accepted" tests assert it did not.
bool hasFfiTypeError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not permitted at the FFI boundary") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Accepted types (§32.5) — no FFI-type diagnostic.
// ---------------------------------------------------------------------------

// §32.5: integer types and the value primitives are FFI-compatible.
BEZEL_TEST(sema_extern_ffi_type, int_params_and_return_ok_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method abs(n: int): int; end;");
    BEZEL_EXPECT_FALSE(hasFfiTypeError(r));
}

// §32.5: `bool`, `byte`, `float`, `double` are all FFI-compatible primitives.
BEZEL_TEST(sema_extern_ffi_type, value_primitives_ok_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" "
        "public method f(b: bool, n: byte, x: float): double; end;");
    BEZEL_EXPECT_FALSE(hasFfiTypeError(r));
}

// §32.5: a method with no `:` return clause is unit-returning and OK.
BEZEL_TEST(sema_extern_ffi_type, omitted_return_is_unit_ok_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method g(n: int); end;");
    BEZEL_EXPECT_FALSE(hasFfiTypeError(r));
}

// §32.5: the unit type `()` is valid as a return type.
BEZEL_TEST(sema_extern_ffi_type, explicit_unit_return_ok_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method g(n: int): (); end;");
    BEZEL_EXPECT_FALSE(hasFfiTypeError(r));
}

// §32.5: inline arrays `[N]T` of an FFI-compatible primitive are admitted.
BEZEL_TEST(sema_extern_ffi_type, inline_array_of_primitive_ok_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method h(buf: [4]int): int; end;");
    BEZEL_EXPECT_FALSE(hasFfiTypeError(r));
}

// §32.5 / §32.8: a function-type (callback) parameter whose params and return
// are themselves FFI-compatible is admitted.
BEZEL_TEST(sema_extern_ffi_type, callback_of_compatible_types_ok_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" "
        "public method reg(cb: (int, int) -> int): int; end;");
    BEZEL_EXPECT_FALSE(hasFfiTypeError(r));
}

// ---------------------------------------------------------------------------
// Rejected types (§32.5 "Not allowed") — FFI-type diagnostic fires.
// ---------------------------------------------------------------------------

// §32.5: Bezel `string` is not allowed (use `CString`/`WString`).
BEZEL_TEST(sema_extern_ffi_type, string_param_rejected_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method foo(s: string): int; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}

// §32.5: `string` as a return type is not allowed.
BEZEL_TEST(sema_extern_ffi_type, string_return_rejected_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method foo(n: int): string; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}

// §32.5: `char` is excluded (use `int`/`byte` and convert).
BEZEL_TEST(sema_extern_ffi_type, char_param_rejected_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method foo(c: char): int; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}

// §32.5: a user class is not an FFI-boundary type.
BEZEL_TEST(sema_extern_ffi_type, user_class_param_rejected_32_5) {
    auto r = parseResolve(
        "module M; "
        "class Widget public method M() return; end M; end Widget; "
        "extern \"c\" public method bar(w: Widget): int; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}

// §32.5: a user class as the return type is not an FFI-boundary type.
BEZEL_TEST(sema_extern_ffi_type, user_class_return_rejected_32_5) {
    auto r = parseResolve(
        "module M; "
        "class Widget public method M() return; end M; end Widget; "
        "extern \"c\" public method bar(n: int): Widget; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}

// §32.5: the unit type `()` is a return-only type — not admitted as a
// parameter.
BEZEL_TEST(sema_extern_ffi_type, unit_param_rejected_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" public method bad(x: ()): int; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}

// §32.5 / §32.8: a function-type (callback) parameter carrying a non-FFI type
// (`string`) is rejected — its parameters must be FFI-compatible.
BEZEL_TEST(sema_extern_ffi_type, callback_with_string_param_rejected_32_5) {
    auto r = parseResolve(
        "module M; extern \"c\" "
        "public method reg(cb: (string) -> int): int; end;");
    BEZEL_EXPECT_TRUE(hasFfiTypeError(r));
}
