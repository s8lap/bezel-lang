// fix #6565 / spec §5.1 (v0.338) — closing the primitive → built-in VALUE
// record fall-through hole in `ExprTC::checkAssignable`.
//
// THE BUG. `Array<T>.Add(item: T)` (stdlib §4.4) is checked by
// `checkAssignable(argTy, itemTy)` via the §22.6 argument-position path. §5.1's
// implicit-conversion TABLE is closed and lists only numeric rows; its closed
// "Reference conversions" list admits a primitive only to `Object`, to a
// class base, to an interface it conforms to, and through §12.9 variance. No
// row converts `int` — or any other concrete primitive — to `DateTime`,
// `TimeSpan`, `Guid`, `IntPtr`, `ReadOnlyStringSpan` or a stdlib §4.18
// synthetic entry record, and each of those is a DIFFERENT record layout from
// the primitive's.
//
// It nevertheless reached `checkAssignable`'s catch-all accept, because every
// arm above the fix is keyed on something these targets lack: the §1178
// mixed class/primitive gate requires `declaration != nullptr` and these carry
// `declaration == nullptr` by construction (#1463); the Named-Named gate
// requires `from->declaration`; the §12.9 variance gate requires
// `to->genericArgs` non-empty, which excludes every non-generic head.
//
// THE HARM (why this is not a benign accept). Codegen lowers an `Add`
// argument through `entryAlloca(v->getType())` — sized from the ARGUMENT's
// LLVM type — but passes the RECEIVER's element stride to `bz_array_add`,
// which memcpy's that stride OUT of the slot. So `Array<DateTime>.Add(5)`
// emitted `call void @bz_array_add(ptr %arr, ptr %1, i64 16)` where `%1 =
// alloca i32, align 4`, and the runtime read 12 bytes of stack redzone:
// `AddressSanitizer: stack-buffer-overflow`, READ of size 16, frame #0
// `memcpy`, frame #1 `bz_array_add` (src/runtime/bezel_runtime_collections.cpp
// :310), rc=99. The rows below pin the REJECTION that replaces it, because
// the rejection is the durable contract — the memcpy cannot be reached once
// the pair no longer typechecks.
//
// VERIFY-FIRST, because it changes what this file is allowed to claim: #6565
// was filed as a `Clear → Add → Clear → Clear` stack-buffer-overflow on the
// mutator sequence #6560 rewrote. MEASURED, the `Clear` is not involved. The
// filed repro (`scratch/p6565.bzl` shape: `arr.Add(P.MakeEntry())` where
// `MakeEntry()` returns a real `DictionaryEntry<string, int>`) COMPILES CLEAN
// and RUNS CLEAN, rc=0, before and after this fix. The overflow needs an `int`
// (or other primitive) in the `Add` slot: `arr.Add(d["aa"])`. So the mutator
// sequence is red herring, and #6560's stamp/stride invariant is untouched by
// this commit — nothing in `src/runtime/` or the #6560 codegen changes.
//
// Rows 1–4 pin THE BUG (one per layout family, each a FAIL-WITHOUT pin). Rows
// 5–12 are the false-positive hazards: every §5.1 row and reference conversion
// that must keep compiling, the §5.2 explicit spelling, the §14.2 generic-body
// deferral, the §5.1 nullable lifting, and the #4733 and #6312 diagnostics
// this arm must NOT steal. Diagnostic wording is pinned by SUBSTRING (`§5.1`,
// the target name, and the arm's identifying clause) so a rephrase that keeps
// the rule's token does not silently break the pin.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The fix's diagnostic: "§5.1 has no implicit conversion from a concrete
// primitive to the value type `DateTime` — its table is closed and lists only
// numeric rows, and a primitive boxes only to `Object` and to the interfaces
// it conforms to (§8.6); convert explicitly per §5.2". Pinned by SUBSTRING:
// the §5.1 reference, the arm's distinguishing clause ("concrete primitive"),
// and the named target. `targetName` is the SIMPLE name as
// `namedTypeSimpleName` renders it — the generic arguments are not part of it
// (`DictionaryEntry`, not `DictionaryEntry<string, int>`).
bool hasPrimitiveToValueRecordDiagnostic(const JitResult& r,
                                         std::string_view targetName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("5.1") == std::string::npos) continue;
        if (d.message.find("concrete primitive") == std::string::npos) continue;
        if (d.message.find(std::string(targetName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// #4733's own diagnostic, kept verbatim for the LAST row: "the target is the
// built-in class `CancellationToken` and the source is a value type — §5.1 has
// no implicit conversion between them, and §4.6 forbids …". The #6565 arm sits
// EARLIER in `checkAssignable`'s chain, so without its
// `!isBuiltinClassHead(CancellationToken)` carve-out it would silently steal
// this rejection and rewrite a message #4733's suite pins. Pinned on #4733's
// distinguishing token ("the built-in class").
bool hasBuiltinClassValueTypeDiagnostic(const JitResult& r,
                                        std::string_view builtinName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("the built-in class ") == std::string::npos) continue;
        if (d.message.find("5.1") == std::string::npos) continue;
        if (d.message.find(std::string(builtinName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// #6312's own diagnostic, kept verbatim for ROW 12: "§5.1 has no implicit
// conversion from this source to the built-in generic type `Span<…>`". The four
// span heads are in `isValueBuiltinByName`, but #6312's arm further down the
// chain owns them and its suite pins this string — see the carve-outs' comment
// in ExprTypecheck.cpp.
bool hasBuiltinGenericConversionDiagnostic(const JitResult& r,
                                           std::string_view head) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("to the built-in generic type `" +
                           std::string(head) + "<…>`") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// ROW 1 — THE BUG, simplest shape: a primitive literal into a
// built-in VALUE record's `Array<T>.Add`.
// Pre-fix: accepted, then `bz_array_add` memcpy's 16 bytes out of a
//         4-byte `alloca i32` → ASan stack-buffer-overflow, rc=99.
// Post-fix: rejected at the call site with the §5.1 diagnostic.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           int_literal_into_array_of_datetime_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr := new Array<DateTime>(); "
        "    arr.Add(5); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasPrimitiveToValueRecordDiagnostic(r, "DateTime"));
}

// =====================================================================
// ROW 2 — THE BUG on the stdlib §4.18 synthetic entry record, which is
// NOT in `isValueBuiltinByName` and so is a SEPARATE arm of the fix.
// This is the type #6565's issue text names, on the receiver the issue
// names (`Dictionary<string, int>.ToArray()`), with the primitive `int`
// that actually triggers the overflow.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           int_into_array_of_dictionary_entry_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class P "
        "  public static method MakeDict(): owned Dictionary<string, int> "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    var d := P.MakeDict(); "
        "    var arr := d.ToArray(); "
        "    arr.Clear(); "
        "    arr.Add(d[\"aa\"]); "     // int, not a DictionaryEntry
        "    arr.Clear(); "
        "    arr.Clear(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasPrimitiveToValueRecordDiagnostic(r, "DictionaryEntry"));
}

// =====================================================================
// ROW 3 — CONTROL on the ISSUE'S OWN REPRO, and the row that makes this
// file honest about what it is fixing. `MakeEntry()` returns a real
// `DictionaryEntry<string, int>`, so the whole `Clear → Add → Clear →
// Clear` sequence #6565 was filed against is WELL-TYPED and must still
// COMPILE AND RUN. MEASURED: rc=0 and prints `done` on the BASE too — the
// filed repro never overflowed, which is why the reported mutator sequence
// is a red herring. If this row ever goes red, the fix over-rejects a
// correct program; if the reported crash comes back, it was never here.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           issue_repro_with_a_real_entry_still_runs_spec_4_4) {
    JitResult r = runAot(
        "module M; "
        "class P "
        "  public static method MakeDict(): owned Dictionary<string, int> "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 1); d.Add(\"bb\", 2); d.Add(\"cc\", 3); "
        "    return d; "
        "  end MakeDict; "
        "  public static method MakeEntry(): owned DictionaryEntry<string, int> "
        "    var arr := MakeDict().ToArray(); "
        "    return arr[0]; "
        "  end MakeEntry; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var arr := P.MakeDict().ToArray(); "
        "      arr.Clear(); "
        "      arr.Add(P.MakeEntry()); "
        "      arr.Clear(); "
        "      arr.Clear(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// =====================================================================
// ROW 4 — THE BUG at the WRITTEN-DECLARATED-TYPE position, not only at a
// call argument: §17.7's `var x: T := e` initializer reaches the same
// `checkAssignable` pair. A primitive initializing a nullable value record
// (`DateTime?`) is rejected too — §5.1's nullable lifting lifts the
// TABLE rows and the reference list, and neither carries `int → DateTime`,
// so there is nothing to lift. (The `null → T?` half is ROW 9.)
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           primitive_initializer_into_nullable_value_record_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: DateTime? := 5; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasPrimitiveToValueRecordDiagnostic(r, "DateTime"));
}

// =====================================================================
// ROW 5 — CONTROL: the §5.1 TABLE row must survive. `int → long` is the
// canonical numeric widening; the fix's predicate requires a NON-numeric
// built-in-record TARGET, which `long` is not.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           numeric_widening_int_to_long_accepted_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var l: long := 5; "
        "    var f: float := 5; "
        "    var b: byte := 50; "
        "    Console.WriteLine(l); Console.WriteLine(f); "
        "    Console.WriteLine(b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 6 — CONTROL: the §5.1 "Reference conversions" rows a primitive
// DOES have. `int → Object` (boxing, §8.6) and `int → IComparable<int>` /
// `IEquatable<int>` (the compiler-provided conformances of §11.8, admitted
// by the `primitiveBoxesIntoBuiltinIface` arm immediately below the fix)
// are the two ways a primitive legitimately reaches a non-numeric target.
// If either goes red, the arm is over-broad.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           primitive_boxing_and_interface_conformance_accepted_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    var c: IComparable<int> := 5; "
        "    var e: IEquatable<int> := 5; "
        "    Console.WriteLine(o); Console.WriteLine(c); Console.WriteLine(e); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 7 — CONTROL: §5.2 is the SPELLING that works for every head the fix
// now rejects at an implicit position. `IntPtr(5L)` / `UIntPtr(7UL)` are
// §32.7's explicit conversions, and `DateTime` keeps its own constructor
// and statics. Rejecting the implicit form is only defensible if the
// explicit one still compiles and RUNS.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           explicit_conversions_still_accepted_spec_5_2) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var p := IntPtr(5L); "
        "    var u := UIntPtr(7UL); "
        "    var d := DateTime.Now; "
        "    var g := Guid.Empty; "
        "    var t := TimeSpan.Zero; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// =====================================================================
// ROW 8 — CONTROL: the §14.2 generic-body DEFERRAL. Inside `Box<T>`, `T`
// is a Named TypeRef with `declaration == nullptr` and the same head
// spelling as nothing in the target set — the fix excludes type variables
// by name (`refMentionsTypeVariable` / `freeTypeParamNames`) so a generic
// body keeps FAILING OPEN to its instantiations. `Array<T>.Add(v)` with
// `v: T` is the shape #6560 relies on, so this row is load-bearing for
// that fix and not merely decorative: it instantiates at `T = int` and
// `T = string`, both primitives.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           generic_body_defers_to_instantiation_spec_14_2) {
    JitResult r = runAot(
        "module M; "
        "class Box<T> "
        "  public static method Add(v: T): int "
        "    var arr := new Array<T>(); "
        "    arr.Add(v); "
        "    return arr.Count; "
        "  end Add; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Box<int>.Add(7)); "
        "    Console.WriteLine(Box<string>.Add(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// =====================================================================
// ROW 9 — CONTROL: §5.1's "Nullable conversions" half, on BOTH sides.
// `null → T?` is admitted (the fix returns early when both sides are
// nullable, so a `null` literal is not mistaken for a primitive), and
// `int? → long?` lifts the TABLE row (the fix's target test fails for
// `long`).
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           nullable_lifting_and_null_literal_accepted_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: DateTime? := null; "
        "    var i: int? := null; "
        "    var l: long? := i; "
        "    var n: DictionaryEntry<string, int>? := null; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// =====================================================================
// ROW 10 — CONTROL: the #4733 DIAGNOSTIC this arm must not steal.
// `CancellationToken` is a `public record` (stdlib §6.1) and so IS in
// `isValueBuiltinByName`, but it wraps a reference to shared
// cancellation state, so it is also in `isHeapPointerReferenceBuiltins`
// and #4733's arm (further down the chain) already rejects it. The fix's
// `!isBuiltinClassHead` carve-out keeps that verdict AND its wording.
// Both pre-fix and post-fix this program is REJECTED (rc=1) — what this
// row pins is that the REASON is still #4733's, not the new arm's.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           cancellation_token_keeps_the_4733_diagnostic_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var t: CancellationToken := 5; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinClassValueTypeDiagnostic(r, "CancellationToken"));
    BEZEL_EXPECT_FALSE(
        hasPrimitiveToValueRecordDiagnostic(r, "CancellationToken"));
}

// =====================================================================
// ROW 11 — CONTROL: the collection LITERAL position. §5.1's constant-
// narrowing paragraph applies "at every position where this section's
// implicit conversions apply", and a collection-literal element is one of
// them — but only for INTEGER targets ("it additionally requires a
// WRITTEN integer target, so … a non-integer target remain[s] the §4.2
// error"). `Array<int> := [1, 2, 3]` and the same literal spelled
// directly must both keep compiling, while `Array<DateTime> := [...]` has
// no such row.
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           integer_collection_literal_accepted_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    var b: Array<long> := [1, 2, 3]; "
        "    Console.WriteLine(a.Count + b.Count); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// =====================================================================
// ROW 12 — CONTROL: the #6312 DIAGNOSTIC this arm must not steal, for
// the OTHER carve-out. `Span` / `ReadOnlySpan` / `ArraySpan` /
// `ReadOnlyArraySpan` are all four in `isValueBuiltinByName`, and #6312's
// arm further down the chain owns them with its own §5.1 wording, which
// its suite
// (`explicit_type_arg_list_overload_6312.sole_survivor_int_and_class_to_
// span_params_rejected_spec_5_1`) pins. Without the `!isSpanHead` carve-out
// the program is still correctly REJECTED but this arm's wording replaces
// #6312's, and that row goes RED — MEASURED, and the reason the carve-out
// exists. `ReadOnlyStringSpan` is the fifth span and is deliberately NOT
// carved out: it is non-generic, so #6312's arm (guarded on a non-empty
// generic-argument list) can never reach it, and `5` into it is one of the
// unsound pairs this fix closes (row 1's family).
// =====================================================================

BEZEL_TEST(primitive_to_value_builtin_record_hole_6565,
           span_head_keeps_the_6312_diagnostic_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class C "
        "  public method S(x: string): int => 1; "
        "  public method S<V>(x: Span<V>): int => 10; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    Console.WriteLine(c.S<int>(3)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinGenericConversionDiagnostic(r, "Span"));
    BEZEL_EXPECT_FALSE(hasPrimitiveToValueRecordDiagnostic(r, "Span"));
}
