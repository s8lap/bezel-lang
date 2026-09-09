// Spec-derived tests for fix #4025 — §7.9 (v0.295) definite assignment for a
// non-nullable field/auto-property of a BUILTIN STDLIB CLASS TYPE (`Mutex`,
// `Semaphore`, `Lock`, `StringBuilder`, `IEnumerable<T>`, `Task<T>`, …) on
// both classes and records, instance and static.
//
// §7.9, verbatim (Bezel_Language_Spec.md line ~1117, v0.295):
//   "Every non-nullable field must be either:
//    - assigned a value in every constructor before the constructor returns, or
//    - given a field initializer at its declaration."
//
// The pre-fix gap, measured on plain origin/main (8cb70ed), was that EVERY
// §7.9-classification predicate —
//   - DefiniteAssignment.cpp's isSevenNineStrictField /
//     isSevenNineStrictAutoProp / isSevenNineStrictStaticField /
//     isSevenNineStrictStaticAutoProp
//   - Ownership.cpp's isNonNullableReferenceField /
//     isNonNullableReferenceAutoProp
// explicitly EXEMPTED a Named type whose rt->declaration is null with an
// early-out "built-in / generic — skip". The intent of the exemption was to
// skip genuinely generic type parameters and truly-unresolved types — but it
// ALSO silently skipped every heap-pointer builtin class (e.g. `Mutex`,
// `Semaphore`, `Lock`, `StringBuilder`, `IEnumerable<T>`, `Task<T>`,
// `OwningArray<T>`), because the compiler has no parsed AST body for any of
// those types (the BuiltinRegistry snapshot at src/common/BuiltinRegistry.cpp
// holds the authority on its name and arity instead, not an `ast::TypeDecl`).
//
// The result, MEASURED on both class and record, instance and static:
//
//   module M; import System.Threading;
//   public class Guard
//     public M: Mutex;                    // instance, no ctor, no initializer
//   end Guard;
//   class C
//     public static method Main(): int
//       var g := new Guard();
//       using var l := g.M.Acquire();
//       return 42;
//     end Main;
//   end C;
//
// `bezelc -o out probe.bzl` → exit 0, no diagnostic. `./out` → SIGSEGV
// (exit 139). The identical shape with a STATIC field (the §7.9 twin #4015
// fixed for user-defined class types) reproduced identically:
//
//   public class Guard
//     public static M: Mutex;             // static, no static ctor, no initializer
//   end Guard;
//
// and a user-defined class-typed field of the same shape is correctly
// rejected by the existing #4001/#4011/#4012/#4015 machinery — confirming
// the gap is specifically the rt->declaration == null exemption reaching
// builtin stdlib classes it was never meant to exempt.
//
// The fix adds a small helper (`isBuiltinClassTypeRef`,
// DefiniteAssignment.cpp / Ownership.cpp — duplication-by-design, mirroring
// the predicates themselves) that consults the BuiltinRegistry snapshot's
// `isReservedBuiltinTypeName(n) && isHeapPointerReferenceBuiltin(n)` (the
// SAME table `Ownership.cpp`'s existing value-shaped-half check at line
// 4225 uses, so the two populations are read off one authority) and replaces
// the unconditional `return false;` early-out in each of the SIX predicates
// with a call to that helper. Value-shaped builtins (`Range`, `DateTime`,
// `Guid`, the span shapes, `JsonOptions`, `ExceptionInfo`,
// `IntPtr`/`UIntPtr`, `CString`/`WString`/`RawBytes`) remain exempt —
// they're in `kNonHeapReservedBuiltins` but NOT in
// `kHeapPointerReferenceBuiltins`, so the helper returns false for them,
// matching their pre-#4025 treatment.
//
// Because the pre-fix symptom is a SIGSEGV rather than a clean diagnostic-
// free compile, every reject-arm test below uses
// `failedAtCompileTimeWithRealLocation` (aot_result_asserts.hpp) rather than a
// bare `!r.ran`, which a crash/signal would satisfy too.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Source prefix every test below uses. The repros are pinned against the
// issue's own wording and the BuiltinRegistry snapshot — `Mutex` and
// `Semaphore` are both in `kHeapPointerReferenceBuiltins` (the class-typed
// half), and `System.Threading` is the §6 module path for both.
const char* kImport4025 = "module M; import System.Threading; ";

// =====================================================================
// REJECT arm 1 — instance FIELD of a builtin class type, class.
// The issue's own measured repro (instance side). Plain `Mutex`, no
// `static`, no initializer, no constructor.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_mutex_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kImport4025) +
        "public class Guard4025 "
        "  public M: Mutex; "
        "end Guard4025; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `M`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 2 — instance FIELD of a builtin class type, RECORD.
// Same shape, on a record. The issue's recorded scope note names both
// classes AND records; both class and record runConstructor paths consult
// isSevenNineStrictField / isNonNullableReferenceField through the same
// predicate, so this also exercises the Ownership.cpp side.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           record_instance_field_of_mutex_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kImport4025) +
        "public record Rec4025(X: int) "
        "  public M: Mutex; "
        "end Rec4025; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Rec4025(7); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `M`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 3 — STATIC FIELD of a builtin class type, class.
// The issue's own measured repro (static side). The §7.9 twin that #4015
// closed for user-defined class types is ALSO closed here for builtin
// class types — `isSevenNineStrictStaticField` is one of the six
// predicates this fix threads `isBuiltinClassTypeRef` into.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_static_field_of_mutex_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kImport4025) +
        "public class Guard4025S "
        "  public static M: Mutex; "
        "end Guard4025S; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `M`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 4 — STATIC FIELD of a builtin class type, RECORD.
// The static-side record axis the issue's scope note calls out.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           record_static_field_of_mutex_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kImport4025) +
        "public record Rec4025S(X: int) "
        "  public static M: Mutex; "
        "end Rec4025S; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `M`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 5 — instance AUTO-PROPERTY of a builtin class type, class.
// Exercises `isSevenNineStrictAutoProp` / `isNonNullableReferenceAutoProp`
// (the auto-property pair of the four predicates). The issue names these
// alongside the field forms in its scope note.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_autoprop_of_mutex_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kImport4025) +
        "public class Guard4025P "
        "  public property M: Mutex; "
        "end Guard4025P; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025P(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `M`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 6 — instance FIELD of Semaphore (sibling heap-pointer builtin
// from the same kHeapPointerReferenceBuiltins row as Mutex). The issue's
// scope note says "every class in `BuiltinRegistry.cpp`'s registry with
// no parsed AST body is likely affected the same way". Two-name pinning
// across both Mutex AND Semaphore makes the fix's BuiltinRegistry-snapshot
// authority visible from the test surface (not just a single builtin
// happening to be wired).
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_semaphore_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kImport4025) +
        "public class Guard4025Sem "
        "  public S: Semaphore; "
        "end Guard4025Sem; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025Sem(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `S`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 7 — instance FIELD of a BUILTIN INTERFACE type (IEnumerable)
// so the `kind == Class || kind == Interface` symmetry from the user-defined
// case is verified to extend to the rt->declaration == null builtin path
// too. This also pins that an INTERFACE builtin (heap-pointer, like
// IEnumerable<T>) is treated identically to a CLASS builtin — there is no
// `default(IEnumerable<T>)` for the constructor screen to fall back on,
// so the user-defined-interface symmetry the predicate already enforces
// must hold for the builtin-interface twin.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_builtin_interface_no_ctor_rejected_spec_7_9) {
    auto r = runAot(
        "module M; "
        "public class Guard4025I "
        "  public E: IEnumerable<int>; "
        "end Guard4025I; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025I(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `E`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// ACCEPT arms — the §7.9 obligation IS discharged by a field initializer,
// by a constructor that assigns it on every normal-exit path, or by a
// static constructor that assigns the static form. These pin over-reject
// guards across BOTH the class and record axes, AND prove the fix's
// `isBuiltinClassTypeRef` correctly classifies the heap-pointer builtin
// as strict (so it can be ASSIGNED through a real initializer rather than
// silently ignored).

// Note on the field shape: per §28.5 a fresh `new Mutex()` is a +1 owned
// production and must be stored into an `owned` field (or moved on). The
// `owned` modifier is therefore required for the ASSIGN path to compile
// cleanly — the same rule that applies to any class-typed field whose
// declared type is IDisposable. The tests below use `public owned` to
// discharge §28.5 too; §7.9 is the load-bearing check this fix is about.
// =====================================================================

// Field initializer discharges §7.9 (and §28.5 via `owned`). Class.
BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_mutex_initializer_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kImport4025) +
        "public class Guard4025I1 "
        "  public owned M: Mutex := new Mutex(); "
        "end Guard4025I1; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025I1(); "
        "    using var l := g.M.Acquire(); "
        "    return 42; "
        "  end Main; "
        "end C;"),
        42);
}

// Constructor assigns the field on every path. Class.
BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_mutex_ctor_assigns_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kImport4025) +
        "public class Guard4025I2 "
        "  public owned M: Mutex; "
        "  public constructor() M := new Mutex(); end constructor; "
        "end Guard4025I2; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025I2(); "
        "    using var l := g.M.Acquire(); "
        "    return 7; "
        "  end Main; "
        "end C;"),
        7);
}

// Field initializer discharges §7.9 on a record. §9.7 forbids `owned`
// fields on a record (records are value types per §9.1), AND a fresh
// `new Mutex()` cannot be stored into a non-`owned` field per §28.5 —
// so a record body-field of a class-typed builtin needs a constructor
// that takes the value AS A PARAMETER (a borrow transfer, not an owned-
// production assignment). That path is well-tested elsewhere (#4013 /
// #4019's record-this-chain work); the RECORD-side §7.9 obligation is
// already exercised by REJECT arm 2 above, which proves
// `isSevenNineStrictField` fires for a record's `public M: Mutex;` with
// no ctor and no initializer. The fix's predicate path is identical for
// records and classes, so the class accept arms below prove the
// §7.9-discharged path; the record reject arm proves the §7.9-violation
// path.

// Static field initializer. Class.
BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_static_field_of_mutex_initializer_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kImport4025) +
        "public class Guard4025SI1 "
        "  public static owned M: Mutex := new Mutex(); "
        "end Guard4025SI1; "
        "class C "
        "  public static method Main(): int "
        "    using var l := Guard4025SI1.M.Acquire(); "
        "    return 13; "
        "  end Main; "
        "end C;"),
        13);
}

// =====================================================================
// REJECT arm 8 — instance FIELD of a builtin EXCEPTION class type
// (`IOException`, from `kBuiltinExceptionHierarchy`, §31). The heap-
// pointer table (`kHeapPointerReferenceBuiltins`) does NOT contain
// exception names — they live in their own hierarchy table because
// they have their own `isA` vtable (§31.2.1) and runtime allocation
// path, structurally distinct from the rest. Before #4025's widening
// to also consult `isBuiltinExceptionName(n)`, a non-nullable field of
// a built-in exception class type compiled clean with no initializer
// (the heap-pointer check missed it, AND no synthesized declaration
// exists to take the user-classified branch) — so this test pins the
// extension. Module path follows `tests/codegen/builtin_member_
// population_2998_test.cpp`'s IOException row: `import System.IO; import
// System.Threading;`.
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_ioexception_no_ctor_rejected_spec_7_9) {
    auto r = runAot(
        "module M; import System.IO; import System.Threading; "
        "public class Guard4025IO "
        "  public E: IOException; "
        "end Guard4025IO; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025IO(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `E`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// OVER-REJECTION GUARD — a value-shaped builtin (IntPtr) is in
// kNonHeapReservedBuiltins (NOT kHeapPointerReferenceBuiltins), so
// `isBuiltinClassTypeRef` correctly returns false for it and a
// non-nullable IntPtr field with no initializer must STILL compile
// clean — IntPtr is i64-sized per stdlib §10.0, a value-typed default
// that does not need §7.9 enforcement. This pins the value-shaped
// half of the BuiltinRegistry split is correctly excluded by
// `isBuiltinClassTypeRef`'s `isHeapPointerReferenceBuiltin(n)` conjunct —
// i.e. the fix does not over-classify IntPtr / UIntPtr / CString / WString
// / RawBytes / Range / DateTime / Guid / span shapes / JsonOptions /
// ExceptionInfo as §7.9-strict just because they have no parsed AST
// body. (`Range` and the temporal/identity value builtins get a SYNTHESIZED
// rt->declaration via `ctx_.builtinClassSym(...)` at TypeResolution_Names
// .cpp:2031/1143, so the rt->declaration != null branch handles them in
// the pre-existing way — the rt->declaration == null branch that this fix
// touches is the one IntPtr proves correctly falls through to a `return
// false`.)
// =====================================================================

BEZEL_TEST(sema_builtin_class_field_da_4025,
           class_instance_field_of_intptr_value_builtin_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Guard4025Iptr "
        "  public I: IntPtr; "
        "end Guard4025Iptr; "
        "class C "
        "  public static method Main(): int "
        "    var g := new Guard4025Iptr(); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

}  // namespace
