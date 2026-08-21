// fix #2831 — sema: `CString.Length` / `WString.Length` are untyped on EVERY
// receiver shape, and `RawBytes.Length` is untyped on a PARAMETER receiver
// (stdlib §10.1 / §10.2 / §10.3, v0.240).
//
// The sibling `Pointer` arm in the same §10 readonly-property block at
// `src/sema/ExprTypecheck_Calls.cpp:14864` already consults
// `common::isFfiBufferBuiltinName` and produces a typed result (IntPtr) on
// BOTH the FACTORY result shape (`CString.From(s)` carries declaration =
// builtinClassSym) and the ANNOTATION shape (`c: CString` on a parameter or
// field, declaration = null, head name only). The `Length` arm at line 14897
// was a NARROW `tt->declaration->name == "RawBytes" &&
// tt->declaration->moduleName.empty()` test — `RawBytes` alone, FACTORY result
// shape alone — so on every other case the expression carried no exprType.
//
// The codegen `Length` arm at `src/codegen/LlvmBackend_Exprs.cpp:3617` reads
// the TARGET type (which IS typed for both shapes) via `isFfiBufferTypeRef`,
// so the runtime VALUE on every shape is correct. The defect was purely on
// the sema view, where the §12.7 concrete-value-type static-type-test at
// `src/codegen/LlvmBackend_Layout.cpp:2749` (`if (operandTy &&
// concreteValueTypeOperand(operandTy))` — `is int` is decided STATICALLY
// from the operand's declared type) silently returned FALSE when the operand
// was untyped: `c.Length is int` was a zero-result on every shape that the
// narrow predicate missed, even though the value is int. Measured on the
// pre-fix predicate: `if c.Length is int then return 7; end; return 0;`
// exited 0 (zero), proving the operand was untyped at sema so the
// §12.7 / #3120 static branch never fired; the test fell through to the
// runtime path's boxed-pointer descent and read the wrong descriptor.
//
// The widening mirrors the sibling: the same `isFfiBuf` predicate the
// `Pointer` arm computed, branched on `ma.member == "Length"`, and stamped
// `int` (§10.1 / §10.2 / §10.3 all declare `Length: int` verbatim; the
// issue brief's "(int / int / long)" is wrong for RawBytes — the spec is
// `int`). The user-type exclusion is preserved (a `declaration->decl !=
// nullptr` is a real user TypeDecl, so a `class CString` / `class WString`
// / `class RawBytes` user class with its own `Length` member — #3022 —
// keeps its own typing) and the user-class pre-empt rows pin that.
//
// FAIL-WITHOUT PROOF, MECHANICAL. With the predicate reverted to the
// pre-fix narrow RawBytes-only / declaration-only test:
//   * Parameter-receiver `c.Length is int` on ALL THREE types exits 0
//     (the operand is untyped, the static-decision branch never fires,
//     the codegen descends to the boxed-pointer path and silently emits
//     the wrong descriptor).
//   * The user-class pre-empt rows still pass on the pre-fix predicate
//     (the user's own `Length` method returns 99/88/77 and dispatches).
// With the post-fix `isFfiBuf` predicate:
//   * Parameter-receiver `c.Length is int` exits 7 on all three types.
//   * The value side (the runtime `return c.Length` rows) is unchanged
//     because the codegen `Length` arm already handled both shapes via
//     `isFfiBufferTypeRef` — the widening is value-preserving.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// True iff `r`'s diagnostics include a compile error (any kind).
bool hasAnyError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == ::bezel::Severity::Error)
            return true;
    }
    return false;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// §10.1 CString.Length — parameter-receiver TYPE-test pin.
//
// The test `c.Length is int` is decided STATICALLY at codegen
// (`LlvmBackend_Layout.cpp:2749`) — the concrete-value-type operand's
// `staticTypeIdentical(operandTy, rt)` is the predicate. With the
// pre-fix narrow predicate, `c.Length` on a parameter receiver was
// untyped at sema, so the operand's `exprType` was null, the static
// branch was skipped, and the code fell through to the runtime path
// (which descends to the boxed-pointer descriptor and emits a
// descriptor-chain walk on a non-pointer value — codegen-level
// silent wrongness, the worst failure mode for a `is T` test).
// Post-fix the operand is typed `int`, `staticTypeIdentical(int, int)`
// is true, and the test returns 7.
// per stdlib §10.1, spec §12.7, v0.240
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           cstring_length_param_is_int_spec_10_1) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Reads(c: CString): int "
        "    if c.Length is int then return 7; end; "
        "    return 0; "
        "  end Reads; "
        "  public static method Main(): int "
        "    using var c := CString.From(\"hello\"); "
        "    return Reads(c); "
        "  end Main; "
        "end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2831 CString param is-int: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §10.2 WString.Length — parameter-receiver TYPE-test pin.
BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           wstring_length_param_is_int_spec_10_2) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Reads(w: WString): int "
        "    if w.Length is int then return 7; end; "
        "    return 0; "
        "  end Reads; "
        "  public static method Main(): int "
        "    using var w := WString.From(\"hello\"); "
        "    return Reads(w); "
        "  end Main; "
        "end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2831 WString param is-int: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §10.3 RawBytes.Length — parameter-receiver TYPE-test pin. This is the
// UNIQUE case the narrow predicate missed on the parameter / annotation
// shape (the predicate required `tt->declaration`, which is non-null only
// on the factory-result shape).
// per stdlib §10.3, spec §12.7, v0.240
BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           rawbytes_length_param_is_int_spec_10_3) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Reads(r: RawBytes): int "
        "    if r.Length is int then return 7; end; "
        "    return 0; "
        "  end Reads; "
        "  public static method Main(): int "
        "    var data: Array<byte> := [104, 105]; "
        "    using var r := RawBytes.From(data); "
        "    return Reads(r); "
        "  end Main; "
        "end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2831 RawBytes param is-int: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// VALUE ROW — runtime value (codegen side).
//
// The codegen `Length` arm at `LlvmBackend_Exprs.cpp:3617` reads the
// target's type via `isFfiBufferTypeRef` and works on BOTH shapes, so the
// runtime value is correct on every shape regardless of the sema
// predicate. This row pins the value side; combined with the `is int`
// rows above, it pins that the fix does not regress the codegen path
// (the widen is VALUE-PRESERVING for the parameter / annotation
// shape, where the pre-fix `Pointer` arm had a sibling codegen defect
// in #2825).
// per stdlib §10.1 / §10.2 / §10.3, v0.240
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           cstring_length_value_param_spec_10_1) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Reads(c: CString): int "
        "    return c.Length; "
        "  end Reads; "
        "  public static method Main(): int "
        "    using var c := CString.From(\"hello\"); "
        "    var n: int := Reads(c); "
        "    if n = 5 then return 7; end; "
        "    return 11; "
        "  end Main; "
        "end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2831 CString param value: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           wstring_length_value_param_spec_10_2) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Reads(w: WString): int "
        "    return w.Length; "
        "  end Reads; "
        "  public static method Main(): int "
        "    using var w := WString.From(\"hello\"); "
        "    var n: int := Reads(w); "
        "    if n = 5 then return 7; end; "
        "    return 11; "
        "  end Main; "
        "end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2831 WString param value: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           rawbytes_length_value_param_spec_10_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Reads(r: RawBytes): int "
        "    return r.Length; "
        "  end Reads; "
        "  public static method Main(): int "
        "    var data: Array<byte> := [104, 105]; "
        "    using var r := RawBytes.From(data); "
        "    var n: int := Reads(r); "
        "    if n = 2 then return 7; end; "
        "    return 11; "
        "  end Main; "
        "end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2831 RawBytes param value: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ─────────────────────────────────────────────────────────────────────────────
// PRE-EMPT: user-class with its own `Length` member (fix #3022).
//
// A user class `class CString { method Length(): int … }` with its own
// `Length` method would have its return typed by the widened branch
// and short-circuit the user's real method dispatch. The pre-fix
// #3022 gate (the `moduleName.empty()` clause) excluded the user
// class only on the RawBytes path; the post-fix widener uses the
// same predicate the `Pointer` arm uses (`common::isFfiBufferBuiltinName`,
// which has its own user-type exclusion — `!tt->declaration->decl` is a
// real user TypeDecl). These rows pass on both pre-fix and post-fix
// and prove the widening does not swallow a user class.
// per spec §11.3, stdlib §10.1, v0.240
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           user_cstring_length_uses_user_method_spec_11_3) {
    auto r = runAot(
        "module M; "
        "class CString "
        "  public method Length(): int "
        "    return 99; "
        "  end Length; "
        "end CString; "
        "class C public static method Main(): int "
        "  var b := new CString(); "
        "  return b.Length(); "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 99) {
        std::cout << "    #2831 user CString: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           user_wstring_length_uses_user_method_spec_11_3) {
    auto r = runAot(
        "module M; "
        "class WString "
        "  public method Length(): int "
        "    return 88; "
        "  end Length; "
        "end WString; "
        "class C public static method Main(): int "
        "  var b := new WString(); "
        "  return b.Length(); "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 88) {
        std::cout << "    #2831 user WString: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

BEZEL_TEST(sema_ffi_buffer_length_typed_2831,
           user_rawbytes_length_uses_user_method_spec_11_3) {
    auto r = runAot(
        "module M; "
        "class RawBytes "
        "  public method Length(): int "
        "    return 77; "
        "  end Length; "
        "end RawBytes; "
        "class C public static method Main(): int "
        "  var b := new RawBytes(); "
        "  return b.Length(); "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 77) {
        std::cout << "    #2831 user RawBytes: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}
