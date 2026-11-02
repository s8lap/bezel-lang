// Spec-derived regression test for issue #7958 (campaign builtin-call-admission,
// lane C9): the built-in CONSTRUCTOR surface of `JsonOptions` and of the hash
// collections' capacity position was unchecked.
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// stdlib §2.15 (Concrete serialization formats) declares, verbatim:
//
//     public record JsonOptions(
//       Indented: bool,                 // pretty-print (serializer); ignored on read
//       IndentWidth: int,               // spaces per level when Indented; 2 when no options are passed
//       AllowTrailingCommas: bool,      // deserializer leniency; false when no options are passed
//       IgnoreUnknownFields: bool,      // deserializer: skip input fields that match
//                                       //   no declared component instead of raising;
//                                       //   false (strict) when no options are passed
//       MaxDepth: int                   // nesting guard; 64 when no options are passed
//     );
//     // All five components are required: a positional record admits no default values
//     // (spec A.3 PositionalField), so `new JsonOptions(...)` supplies every one. The
//     // values above are what the no-options constructors behave as.
//
// spec §17.7 (The `new` operator), "Standard-library builtins have a closed
//   constructor surface": "For a standard-library builtin type -- any type the
//   standard-library document declares -- the constructor surface is exactly
//   what that document declares -- `new T(...)` resolves only against a declared
//   constructor, never against a compiler-supplied fallback. ... For a builtin
//   record whose only declared constructor is its positional primary
//   constructor ..., the undeclared parameterless form `new T()` is rejected the
//   same way -- exactly as for an identically-shaped user record."
// spec A.3 `PositionalField ::= Identifier ":" Type SerializedClause?
//   EqualityClause?` -- a positional field admits no default, so every one of
//   the five components is a REQUIRED parameter.
// spec §22.5 (Optional parameters and named arguments): "Every parameter that
//   declares no default must be bound, positionally or by name; leaving one
//   unbound ... makes the candidate **inapplicable** (§22.6)".
// spec §5.1 (Implicit conversions), the never-implicit list: "The following are
//   **never implicit**: ... - Between integers and `bool`." and, in the
//   constant-narrowing clause, "An expression of integer type that is a
//   **compile-time constant** ... implicitly converts to any **integer type**
//   whose range contains its value" -- a `bool` is not an integer type, so the
//   clause never reaches a `bool` position (and §5.2 says so of the explicit
//   form too: "`bool(n)` remains an error in both the implicit and explicit
//   forms").
// spec §5.1, the never-implicit list, first bullet: "Between signed and
//   unsigned types of the same or narrower width (e.g. `int → uint`,
//   `uint → int`, `short → ushort`) ... is never implicit."
// spec §7.1 / §7.3: a nullable value does not implicitly convert to its
//   non-nullable form, and "Assigning `null` to a non-nullable target is a
//   compile error."
// stdlib §4.8 (`Dictionary<K, V>`), §4.9 (`OwningDictionary<K, V>`) and §4.10
//   (`Set<T>` and `OwningSet<T>`) declare, each:
//     public constructor();
//     public constructor(capacity: int);
//     public constructor(comparer: IEqualityComparer<K>);        // §4.10: <T>
//     public constructor(capacity: int, comparer: IEqualityComparer<K>);
//   -- the `capacity` parameter is an `int`, and the `comparer` overload is a
//   NAMED (interface) type, so the two are distinguishable by the argument's
//   own type.
//
// --- THE DEFECTS (MEASURED on r30-c1 = 22343a59) -----------------------------
//
// 1. `new JsonOptions()` , `new JsonOptions(true, 2)` , `new JsonOptions(true,
//    2, false, false)` and the six-argument form ALL COMPILED AND RAN: the
//    arity was never checked, and `lowerNew` filled the missing components
//    with its own constants and DROPPED the surplus one.
// 2. `new JsonOptions(1L, 2, false, false, 3)` -- a `long` at the `bool`
//    `Indented` component -- COMPILED AND RAN (and a `double` there CRASHED
//    codegen with an LLVM "Cannot select" error). Only the two `int`
//    components carried a type check.
// 3. `new Set<int>(u)` over `u: uint` was refused with the WRONG reason --
//    "stdlib §4.10: the 'Set' IEqualityComparer constructor is not yet
//    implemented" -- because the capacity rows of the parameter table carried
//    NO type opinion, so the §5.1 gate never reached the position and codegen's
//    own comparer discriminator classified every non-`int` integral as a
//    comparer. The same position also accepted an un-narrowed `int?` with NO
//    diagnostic at all.
//
// The three `new JsonOptions()` spellings in OTHER test programs are the named
// flips of this fix; they are listed in the commit body.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A program with the serialization imports `JsonOptions` needs.
std::string jsonProg(const std::string& body) {
    return "module M; import System; import System.Collections; "
           "import System.Text; import System.Serialization; "
           "record P(X: int, Y: int): ISerializable, IDeserializable; "
           "class C "
           "  public static method Id(x: int): int? return x; end Id; "
           // The serializer's byte count is how the options are OBSERVED: a
           // `JsonOptions` MEMBER read does not lower (a pre-existing #2434 /
           // #3079 gap, not this fix's business), so a conformance case for
           // stdlib §2.15 must read the options through the serializer.
           "  public static method Len(o: JsonOptions): int "
           "    using var s := new JsonSerializer(o); "
           "    var p := new P(3, 7); p.Serialize(s); "
           "    using var bytes := s.ToBytes(); return bytes.Length; "
           "  end Len; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program over the hash collections (`System.Collections.Generic`).
std::string setProg(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C "
           "  public static method Id(x: int): int? return x; end Id; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, needle.c_str());
}

// The §17.7 arity text: the house wording fix #2994 grew and fix #3136
// generalised, which `ExceptionInfo` (§31.4) and `ReadOnlyStringSpan`
// (stdlib §3.3) use.
const char* kArityText = "is not a declared constructor of `JsonOptions`";

}  // namespace

// --- DEFECT 1: the ARITY of `JsonOptions` (stdlib §2.15, spec §17.7) ---------

// per stdlib §2.15 ("All five components are required ... so `new
// JsonOptions(...)` supplies every one") + spec §17.7 (the closed constructor
// surface): an argument list of TWO components matches no declared form.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           json_options_arity_too_few_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true, 2);"),
        kArityText));
    // The diagnostic names the EXPECTED count (five positional components).
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true, 2);"),
        "which takes 5 positional fields"));
    // FOUR components is the same defect.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true, 2, false, false);"),
        kArityText));
    // ONE component is the same defect, and the diagnostic is in agreement
    // with its own count ("1 argument does not match it").
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true);"),
        "so the supplied 1 argument does not match it"));
}

// per stdlib §2.15 + spec §17.7 ("the undeclared parameterless form `new T()`
// is rejected the same way -- exactly as for an identically-shaped user
// record"): `new JsonOptions()` is not a declared constructor.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           json_options_arity_parameterless_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(jsonProg("    var o := new JsonOptions();"),
                                   kArityText));
    // The parameterless form has its own wording (fix #2994's, kept verbatim).
    BEZEL_EXPECT_TRUE(
        rejectsWith(jsonProg("    var o := new JsonOptions();"),
                    "so the parameterless form is rejected exactly as it "
                    "would be for an identically-shaped user record"));
}

// per stdlib §2.15 + spec §22.5 ("Every parameter that declares no default
// must be bound") + spec §17.7: a SIX-component list binds no declared form
// either, and the surplus argument is not silently dropped.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           json_options_arity_too_many_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true, 2, false, false, 3, 4);"),
        kArityText));
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true, 2, false, false, 3, 4);"),
        "which takes 5 positional fields"));
}

// --- DEFECT 2: the three `bool` components are TYPE-checked (§5.1) -----------

// per spec §5.1 ("Between integers and `bool`" is never implicit) + stdlib
// §2.15: each of the three `bool` components is declared `bool`, and a `long`
// (a CONSTANT or not -- §5.1's constant-narrowing clause reaches only an
// INTEGER target) is not implicitly convertible to one.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           json_options_bool_positions_type_checked) {
    // `Indented`.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var l: long := 1; "
                 "    var o := new JsonOptions(l, 2, false, false, 3);"),
        "the `Indented` argument of the `JsonOptions` constructor has a type "
        "that is not implicitly convertible to the target type (\xC2\xA7""5.1)"));
    // A `long` CONSTANT is refused there too -- §5.1's constant narrowing
    // reaches "any INTEGER type", and a `bool` is not one.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(1L, 2, false, false, 3);"),
        "the `Indented` argument of the `JsonOptions` constructor has a type "
        "that is not implicitly convertible to the target type (\xC2\xA7""5.1)"));
    // `AllowTrailingCommas`.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var l: long := 1; "
                 "    var o := new JsonOptions(true, 2, l, false, 3);"),
        "the `AllowTrailingCommas` argument of the `JsonOptions` constructor "
        "has a type that is not implicitly convertible to the target type "
        "(\xC2\xA7""5.1)"));
    // `IgnoreUnknownFields`.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var l: long := 1; "
                 "    var o := new JsonOptions(true, 2, false, l, 3);"),
        "the `IgnoreUnknownFields` argument of the `JsonOptions` constructor "
        "has a type that is not implicitly convertible to the target type "
        "(\xC2\xA7""5.1)"));
    // The §5.1 reason is NAMED, in the spec's own wording.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(1L, 2, false, false, 3);"),
        "between integers and `bool` is never implicit"));
    // And the `int` components reject a `bool` (the same §5.1 bullet, the
    // other way round) -- unchanged by this fix, pinned as the control.
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(true, true, false, false, 3);"),
        "the `IndentWidth` argument of the `JsonOptions` constructor has a "
        "type that is not implicitly convertible to the target type "
        "(\xC2\xA7""5.1)"));
}

// --- DEFECT 3: the hash collections' CAPACITY position (§5.1) ----------------

// per spec §5.1 ("Between signed and unsigned types of the same or narrower
// width (e.g. ... `uint → int` ...) ... is never implicit") + stdlib §4.10
// (`Set<T>(capacity: int)` beside `Set<T>(comparer: IEqualityComparer<T>)`):
// a `uint` capacity is not a well-typed call against EITHER overload, and the
// diagnostic must say so -- not "the comparer constructor is not yet
// implemented", which was the reason the base drew.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           set_family_capacity_conversion_refused) {
    const char* kCapacity = "the `capacity` argument of the `Set` constructor "
                             "has a type that is not implicitly convertible to "
                             "the target type (\xC2\xA7""5.1)";
    auto r = runAot(setProg(
        "  var u: uint := 5; "
        "  using var s: Set<int> := new Set<int>(u); "
        "  return s.Count; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCapacity));
    // The OLD reason is gone: the base drew the comparer deferral here.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "IEqualityComparer constructor is not yet implemented"));
    // The same for `Dictionary` (stdlib §4.8) and `OwningSet` (§4.10).
    BEZEL_EXPECT_TRUE(rejectsWith(
        setProg("  var u: uint := 5; "
                "  using var d: Dictionary<string, int> := "
                "    new Dictionary<string, int>(u); "
                "  return d.Count; "),
        "the `capacity` argument of the `Dictionary` constructor has a type "
        "that is not implicitly convertible to the target type "
        "(\xC2\xA7""5.1)"));
    // A `long` capacity is the same defect (stdlib §4.10).
    BEZEL_EXPECT_TRUE(rejectsWith(
        setProg("  var n: long := 5; "
                "  using var s: Set<int> := new Set<int>(n); "
                "  return s.Count; "),
        kCapacity));
    // The NAMED spelling of the same position is covered too (§22.5): a named
    // `capacity` binds the capacity parameter, so a `uint` there is the very
    // same §5.1 defect. (The named `comparer` spelling is a DIFFERENT
    // parameter -- see `set_comparer_form_still_diagnosed`.)
    BEZEL_EXPECT_TRUE(rejectsWith(
        setProg("  var u: uint := 5; "
                "  using var s: Set<int> := new Set<int>(capacity := u); "
                "  return s.Count; "),
        kCapacity));
}

// per spec §7.1 / §7.3 (a nullable value does not implicitly convert to its
// non-nullable form) + stdlib §4.10 (`capacity: int` is non-nullable): an
// UN-narrowed `int?` at the capacity is a compile error. The base accepted it
// with NO diagnostic at all.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           set_family_nullable_capacity_refused) {
    auto r = runAot(setProg(
        "  var n: int? := Id(5); "
        "  using var s: Set<int> := new Set<int>(n); "
        "  return s.Count; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the `capacity` argument of the `Set` constructor is nullable but "
           "the target type is non-nullable (\xC2\xA7""7)"));
    // A `null` LITERAL at the capacity is the same defect (spec §7.3: "Assigning
    // `null` to a non-nullable target is a compile error"). The base drew the
    // comparer deferral here; the tip draws §7, which names the real reason.
    BEZEL_EXPECT_TRUE(rejectsWith(
        setProg("  using var s: Set<int> := new Set<int>(null); "
                "  return s.Count; "),
        "the `capacity` argument of the `Set` constructor is nullable but "
        "the target type is non-nullable (\xC2\xA7""7)"));
}

// --- THE CONTROLS: every shape the fix must NOT touch -----------------------

BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           accept_controls) {
    // The legal five-component form COMPILES AND RUNS, and the options reach
    // the serializer (stdlib §2.15).
    BEZEL_EXPECT_EQ(runAotOut(jsonProg(
        "    Console.WriteLine(Len(new JsonOptions(true, 2, false, false, 3)) "
        "                      = Len(new JsonOptions(true, 2, false, false, 3))); "
        "    Console.WriteLine(Len(new JsonOptions(true, 2, false, false, 3)) "
        "                      <> Len(new JsonOptions(true, 9, false, false, 3)));")),
        "True\nTrue");
    // The `int` components still accept §5.1's WIDENINGs (`byte` -> `int`)
    // and its CONSTANT NARROWING (`4L` -> `int`): the fix must not narrow the
    // admitted set at the positions it already checked.
    BEZEL_EXPECT_EQ(runAotOut(jsonProg(
        "    var b: byte := 2; "
        "    Console.WriteLine(Len(new JsonOptions(true, b, false, false, 3)) "
        "                      = Len(new JsonOptions(true, 2, false, false, 3))); "
        "    Console.WriteLine(Len(new JsonOptions(true, 4L, false, false, 32L)) "
        "                      = Len(new JsonOptions(true, 4, false, false, 32)));")),
        "True\nTrue");
    // The capacity forms that ARE well-typed still compile and run: an `int`
    // capacity, a §5.1 widening (`byte`) capacity, and a CONSTANT capacity of
    // a wider integer type -- §5.1's constant narrowing admits "a compile-time
    // constant of ANY integer type ... [at] any integer type whose range
    // contains its value, across signedness" (the base refused the last one
    // with the comparer wording; the fix admits it).
    BEZEL_EXPECT_EQ(runAotOut(setProg(
        "  using var s0: Set<int> := new Set<int>(5); "
        "  s0.Add(1); "
        "  var b: byte := 4; "
        "  using var s1: Set<int> := new Set<int>(b); "
        "  s1.Add(2); "
        "  const n: long := 6; "
        "  using var s2: Set<int> := new Set<int>(n); "
        "  s2.Add(3); "
        "  if s0.Count <> 1 then return 90; end; "
        "  if s1.Count <> 1 then return 91; end; "
        "  if s2.Count <> 1 then return 92; end; "
        "  Console.WriteLine(1); ")),
        "1");
    // A §7.4-NARROWED nullable capacity is legal (§22.6: "a read of a binding
    // §7.4 has narrowed non-null is of its non-nullable type") and runs.
    BEZEL_EXPECT_EQ(runAotOut(setProg(
        "  var n: int? := 7; "
        "  if n is not null then "
        "    using var s: Set<int> := new Set<int>(n); "
        "    s.Add(1); "
        "    if s.Count <> 1 then return 93; end; "
        "  end if; "
        "  Console.WriteLine(1); ")),
        "1");
}

// The deliberate design (fix #7928): `JsonOptions` has NO row in the parameter
// table -- a row would admit a NAMED argument, and `lowerNew` binds the five
// components positionally. The fix must not break that: a named argument is
// still refused, with §22.5's own diagnostic.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           json_options_named_arguments_still_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        jsonProg("    var o := new JsonOptions(Indented := true, "
                 "IndentWidth := 2, AllowTrailingCommas := false, "
                 "IgnoreUnknownFields := false, MaxDepth := 3);"),
        "named arguments are not supported on this built-in constructor"));
}

// The comparer overload of the hash collections is a DIFFERENT declared
// overload (stdlib §4.10: `Set<T>(comparer: IEqualityComparer<T>)`), and
// codegen's deferral for it (fix #2722) is unchanged by this fix: a
// class-typed comparer still draws it.
BEZEL_TEST(builtin_json_options_arity_bool_set_capacity_7958,
           set_comparer_form_still_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; import System.Collections.Generic; "
        "class ModCmp: IEqualityComparer<int> "
        "  public method Equals(a: int, b: int): bool => a mod 10 = b mod 10; "
        "  public method GetHashCode(value: int): int => value mod 10; "
        "end ModCmp; "
        "class C public static method Main(): int "
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var s: Set<int> := new Set<int>(cmp); "
        "  return s.Count; "
        "end Main; end C;",
        "the 'Set' IEqualityComparer constructor is not yet implemented"));
}
