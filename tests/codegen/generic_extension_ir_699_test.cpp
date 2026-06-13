// Codegen IR-validity tests for generic extension method bodies (§26.4,
// issue #699).
//
// Background. §26.4 (Generic extensions): an extension on a generic type
// (`extension IEnumerable<T>`) introduces a block-level type parameter T.
// Per §26.5, every call site monomorphizes the extension to the receiver's
// concrete type; the UNSPECIALIZED template body is a dead stub that is never
// invoked.  Even so, declareFunctions/emitMethodBody emit that stub's body, so
// it must lower to STRUCTURALLY-VALID LLVM IR.
//
// The defect (#699): inside the stub, a LINQ materializer on the `this`
// receiver — `var items := this.ToArray();` — had no sema exprType (the
// IEnumerable<T> receiver is neither the lazy-LINQ sentinel nor a concrete
// collection), so the inferred-local slot defaulted to i32.  `return items`
// then emitted `ret i32` into a function whose declared return type is
// `Array<T>` → `ptr` — a "Function return type does not match operand type of
// return inst!" IR-verification failure.
//
// The fix types the common LINQ materializers/aggregators on an IEnumerable<T>
// receiver in sema (ExprTypecheck_Calls.cpp), so the inferred local gets its
// correct slot type and the stub's `ret` matches the function's return type.
// No codegen IR backstop is used — the IR is valid because the types are right.
//
// Derived from spec §26.4 examples (spec_0165 Sort/ToArray, spec_0166 Map<U>).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::emitAndVerify;

// ---------------------------------------------------------------------------
// §26.4 spec_0165 — `extension IEnumerable<T> where T: IComparable<T>` with
// `Sort(): Array<T>` whose body does `var items := this.ToArray(); return
// items;`.  The emitted module MUST verify (was: ret i32 into a ptr function).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_generic_extension_ir_699, sort_toarray_template_body_verifies_spec0165) {
    auto r = emitAndVerify(
        "module M; "
        "extension IEnumerable<T> where T: IComparable<T> "
        "  public method Sort(): Array<T> "
        "    var items := this.ToArray(); "
        "    return items; "
        "  end Sort; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ---------------------------------------------------------------------------
// §26.4 — the block-level T appears DIRECTLY in the return type (`First(): T`)
// with `var x := this.First(); return x;`.  T erases to the i32 placeholder in
// the stub, so the local slot AND the return type are both i32 — they must
// match (this is the exact return-type-mismatch class a global backstop would
// mask; here it is correct by construction).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_generic_extension_ir_699, bare_T_return_first_verifies_sec26_4) {
    auto r = emitAndVerify(
        "module M; "
        "extension IEnumerable<T> "
        "  public method FirstItem(): T "
        "    var x := this.First(); "
        "    return x; "
        "  end FirstItem; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ---------------------------------------------------------------------------
// §26.4 — an aggregator returning a primitive (`Total(): int` via
// `this.Count()`).  The int-returning shape already verified before the fix;
// kept as a regression guard that typing Count() did not perturb it.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_generic_extension_ir_699, count_int_return_verifies_sec26_4) {
    auto r = emitAndVerify(
        "module M; "
        "extension IEnumerable<T> "
        "  public method Total(): int "
        "    var c := this.Count(); "
        "    return c; "
        "  end Total; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ---------------------------------------------------------------------------
// §26.4 spec_0166 — `Map<U>(transform: (T)->U): IEnumerable<U>` building a
// `new Array<U>()` and returning it.  Both the extension's T and the method's
// own U must lower to valid IR in the stub body.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_generic_extension_ir_699, map_method_u_template_body_verifies_spec0166) {
    auto r = emitAndVerify(
        "module M; "
        "extension IEnumerable<T> "
        "  public method Map<U>(transform: (T) -> U): IEnumerable<U> "
        "    var result := new Array<U>(); "
        "    for var item in this do "
        "      result.Add(transform(item)); "
        "    end; "
        "    return result; "
        "  end Map; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.verifies);
}

// ---------------------------------------------------------------------------
// §26 regression — a non-generic extension whose body returns a value must
// still emit verifiable IR (the fix must not disturb the non-IEnumerable path).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_generic_extension_ir_699, non_generic_extension_still_verifies_sec26_1) {
    auto r = emitAndVerify(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo "
        "  public method Bar(): int => this._x + 1; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.verifies);
}
