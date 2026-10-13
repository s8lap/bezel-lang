// Regression tests for issue #2996 (language spec §11.3 user-declared class
// names / §2.15 stdlib serialization, v0.240):
// an ordinary USER METHOD call on a USER class named BinarySerializer /
// BinaryDeserializer / JsonSerializer / JsonDeserializer did not lower — it
// degraded to a zero return value with only an `internal:` note. §11.3
// reserves no names, so the program is legal and contains no built-in at
// serializer at all.
//
//   class BinarySerializer
//     public readonly property Tag: int := 3;
//     public method Get(): int
//       return this.Tag + 4;
//     end Get;
//   end BinarySerializer;
//   class C
//     public static method Main(): int
//       using var s := new BinarySerializer();
//       return s.Get();        -- expected 7
//     end Main;
//   end C;
//
// Reported diagnostic location: `x.bzl:11:12: internal: return-value
// expression failed to lower to a value (degraded to a zero/null return
// value) — #2491`. The note is at the CALL site, not inside the user class's
// own `Get`, even though the issue's prose put it inside the `this.Tag` read
// — the call site is where the lowered value went missing.
//
// ── THE SCOPE THE ISSUE STATED, AND WHAT MEASUREMENT FOUND ──────────────────
// #2996 named the four §2.15 serializer names and asked: do user METHOD calls
// lower? It is exactly those four. A property read on the same class was fine
// (`s.Tag` from the caller returns 3 correctly), which is why #2991's grid —
// which read a property — did not catch it. A `using var` with one of these
// four names was the OTHER family defect, fixed in #2991.
//
// ── THE FIX: ONE FACT, ASKED ONCE ───────────────────────────────────────────
// `Emitter::isSerializationCall` (LlvmBackend_Serialization.cpp) is the
// lightweight pre-check `lowerCallDispatch` consults before the regular method
// dispatch; on a NAME match it routed the call to `tryLowerSerializationCall`,
// which then refused the call (its own inner guard `!recvTy->declaration`
// skipped the arm because a user class has a real TypeSymbol). The result was
// `lowerCall` returning nullptr for a CALL that had not been handled, so the
// outer `if (call.callee)` arm returned that nullptr too, and the
// return-statement degrader fired with the §2.15-name-flavoured
// `internal: return-value expression failed to lower` note. Without the
// pre-check's own gate the call was diverted before the regular method
// dispatch ever saw it.
//
// The fix extracts the discriminator the inner guard already used (the same
// shape `builtinTypeName` reads in #2997's chain) into a single helper
// `isBuiltinSerializerTypeRef` and applies it to BOTH sides:
//   * the pre-check: a user class with the same name is NOT a serialization
//     call, so the regular method dispatch runs;
//   * the inner guard: unchanged — the helper is the SAME shape the
//     `tryLowerSerializationCall` arm requires, so a built-in still matches.
// `!recvTy->declaration` (synthetic handle) OR `!recvTy->declaration->decl &&
// recvTy->declaration->moduleName.empty()` (builtinClassSym) is the one fact
// the compiler already computes that distinguishes a built-in from a user
// class sharing its name, and an arm added later inherits the guard rather
// than copying its neighbour's defect (the shape #2921/#2967/#2973/#2991
// chose for this same chain).
//
// A silent wrong value is not the worst outcome; a wild free is. The fix
// returns the dispatch to the regular method-dispatch path; for a class
// without a `Get` method, sema rejects the call BEFORE codegen sees it, so
// the wrong-direction outcome is the LOUD one (§11.3's contract, upheld).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A user class of the given name carrying BOTH the value oracle (a method
// that uses `this`) and the call-site path the issue repro traces:
//   * `public readonly property Tag: int := 3;` — what the issue's repro
//     reads; on a user class this MUST return 3 from the caller.
//   * `public method Get(): int` — body reads `this.Tag` and adds 4; this is
//     the call the issue reports as zero.
//   * `public method Sum(a: int, b: int): int` — a second method, with
//     arguments, to make sure the fix is not specific to the zero-arg shape.
std::string userClassMethodCall(const char* name) {
    std::string n = name;
    return
        "module M; "
        "class " + n + " "
        "  public readonly property Tag: int := 3; "
        "  public method Get(): int "
        "    return this.Tag + 4; "
        "  end Get; "
        "  public method Sum(a: int, b: int): int "
        "    return this.Tag + a + b; "
        "  end Sum; "
        "end " + n + "; "
        "class C public static method Main(): int "
        "  var s := new " + n + "(); "
        "  if s.Get() <> 7 then return 1; end; "
        "  if s.Sum(10, 20) <> 33 then return 2; end; "
        "  if s.Tag <> 3 then return 3; end; "
        "  return 7; "
        "end Main; end C;";
}

// The `using var` spelling, with the property read AFTER the using-var
// bind — the issue's own repro. Confirms the fix is not specific to a plain
// `var` and that the using-bind path still produces a callable instance.
std::string userClassUsingVarMethodCall(const char* name) {
    std::string n = name;
    return
        "module M; "
        "class " + n + " "
        "  public readonly property Tag: int := 3; "
        "  public method Get(): int "
        "    return this.Tag + 4; "
        "  end Get; "
        "end " + n + "; "
        "class C public static method Main(): int "
        "  using var s := new " + n + "(); "
        "  if s.Tag <> 3 then return 1; end; "
        "  if s.Get() <> 7 then return 2; end; "
        "  return 7; "
        "end Main; end C;";
}

// Reports WHICH name failed — a grouped test that only says "false" would
// send the next reader back to re-derive the grid.
void expectUserClassMethodCallWorks(const char* name,
                                    const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2996: user class named `" << name
                  << "` method call did not lower"
                  << " (ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

}  // namespace

// ── The filed repro, verbatim ───────────────────────────────────────────────

BEZEL_TEST(user_class_named_builtin_serializer_call_2996,
           user_binary_serializer_method_call_does_not_lower_to_zero_spec_11_3) {
    // #2996's program exactly. On 193af4f: exit=0 and
    // `x.bzl:11:12: internal: return-value expression failed to lower to a
    // value (degraded to a zero/null return value) — #2491`. Plain `var` (no
    // using), to isolate the method-dispatch path from the disposal path
    // (#2991 fixed the latter for the same names; this is the OTHER family
    // defect for these four names).
    auto r = runAot(
        "module M; "
        "class BinarySerializer "
        "  public readonly property Tag: int := 3; "
        "  public method Get(): int "
        "    return this.Tag + 4; "
        "  end Get; "
        "end BinarySerializer; "
        "class C public static method Main(): int "
        "  var s := new BinarySerializer(); "
        "  return s.Get(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// ── The four names, plain `var` bind ────────────────────────────────────────

BEZEL_TEST(user_class_named_builtin_serializer_call_2996,
           the_four_section_2_15_names_method_calls_lower_spec_2_15) {
    // The exact grid the issue measured: the four §2.15 serializer names.
    // A method call on a user class with any of these names MUST return the
    // user-method's value, not the built-in's zero fallback. Pinned with three
    // shapes per name (zero-arg, two-arg, property read) so a future change
    // that fixes the zero-arg shape and regresses the others is caught here.
    for (const char* n : {"BinarySerializer", "BinaryDeserializer",
                          "JsonSerializer", "JsonDeserializer"})
        expectUserClassMethodCallWorks(n, userClassMethodCall(n));
}

// ── The four names, `using var` bind (the issue's literal repro) ────────────

BEZEL_TEST(user_class_named_builtin_serializer_call_2996,
           using_var_method_calls_still_lower_spec_2_15) {
    // The disposal path is #2991's lane; this is the OTHER lane (#2996).
    // The using-var bind path ALSO must not divert method calls into the
    // built-in serializer arm.
    for (const char* n : {"BinarySerializer", "BinaryDeserializer",
                          "JsonSerializer", "JsonDeserializer"})
        expectUserClassMethodCallWorks(n, userClassUsingVarMethodCall(n));
}

// ── The other direction: a real built-in serializer must still work ─────────

BEZEL_TEST(user_class_named_builtin_serializer_call_2996,
           genuine_builtins_still_round_trip_spec_2_15) {
    // `isBuiltinSerializerTypeRef` is a NARROWING of the previous bare-name
    // match, so the risk it carries is the opposite one — a real built-in
    // that stops matching its arm. Measured clean before AND after on a
    // round-trip through the runtime: write int 42 with BinarySerializer,
    // read it back with BinaryDeserializer, assert equality. The bound
    // `using var` keeps the runtime handles alive long enough for the test
    // process to be readable by LSan on the next pass.
    auto r = runAotChecked(
        "module M; "
        "import System.Serialization; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  s.WriteInt(42); "
        "  using var bytes := s.ToBytes(); "
        "  if bytes.Length <> 8 then return 1; end; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  if d.ReadInt() <> 42 then return 2; end; "
        "  return 7; "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #2996: built-in serializer round-trip regressed"
                  << " (ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}
