// Spec-derived regression test for issue #6776.
//
// Spec §8.2 (Runtime type identity), v0.340: an object's runtime type is fixed
// at allocation; the built-in classes are not exempt. Spec §3.1 / §5.1, v0.340:
// a class value reaches `Object` directly (representation-preserving, no box).
// Spec §8.5 (`ToString`), v0.340: for a class with no override, the default is
// "the type's qualified name". Spec §8.3, v0.340: `Equals` and `GetHashCode`
// default to reference identity. Spec §12.7, v0.340: `is` / `as` test the
// runtime type. Spec §19.3, v0.340: a named pattern is the same test. stdlib
// §2.1 (`Console`), v0.340: "The `Object` overloads invoke `ToString()`".
// stdlib §3.1 (`StringBuilder`), v0.340: `ToString()` is the builder's text.
// stdlib §6.2 (`Mutex`), v0.340 and stdlib §4.x (`Array<T>`): neither declares
// `ToString`, so §8.5's default applies ("System.Threading.Mutex",
// "System.Collections.Generic.Array<int>").
//
// THE DEFECT. `StringBuilder`, `Array<T>` and `Mutex` are "vtable-less"
// built-ins: plain runtime structs with no Bezel vtable. Every `Object`-surface
// operation on an `Object` location (ToString, Equals, GetHashCode, is / as,
// `case`, disposal) discriminated string-vs-class with the NEGATIVE test
// `bz_obj_is_string` ("not a registered vtable ⇒ a string"), so such a value
// was read as a `%bz.string`. MEASURED on 279931c7 (the base), ASan armed: the
// issue's repro (`Show(o: Object) Console.WriteLine(o)` with a StringBuilder
// holding "q") printed garbage and crashed (SEGV in bz_array_free); the
// interpolation form the same; `Array<int>` printed an empty line and crashed;
// `Mutex` double-freed; a real string printed "hello" (the control).
//
// THE FIX gives the three built-ins a runtime type tag: word 0 of the struct is
// a runtime-owned vtable in the class slot layout (bz_array_vtable /
// bz_sb_vtable / bz_mutex_vtable), set at every allocation site, with a
// per-instantiation descriptor (`Array<int>` vs `Array<string>`) stamped by
// codegen. `bz_obj_is_string` recognises the tags, and the existing class
// protocol (vtable slots for Dispose / ToString / Equals / GetHashCode, the
// descriptor-chain walk for `is`) dispatches on them.
//
// WHY THE ORACLES ARE WHAT THEY ARE. Every row runs under the checked oracle
// (`runAotChecked`: sanitizers armed, exit code required to be 0) and compares
// the program's exact stdout, so a wrong rendering, a leak, a double free or a
// use-after-free fails the row instead of passing silently. The one-shot shapes
// are looped 200× inside the program so a per-iteration leak is visible to the
// leak oracle.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Checked oracle: the program must run with sanitizers armed and exit 0. The
// value is its stdout, so a wrong rendering or a leak/UAF fails the row.
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

// Plain oracle: the program must compile, run and exit 0; the value is its
// stdout. Used only where a filed, independent leak (#6896) would fail the
// checked oracle for a reason unrelated to the row.
std::string plainOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

// `module M; <prelude> class C <members> public static method Main(): int
// <mainBody> return 0; end Main; end C;`
std::string prog(const std::string& members, const std::string& mainBody,
                 const std::string& prelude = "") {
    return "module M; " + prelude + "class C " + members +
           " public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

std::string rep(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// The issue's `Show`: an `Object`-typed parameter handed to the `Console`
// Object overload (stdlib §2.1).
const char* kShow =
    "  public static method Show(o: Object) Console.WriteLine(o); end Show; ";

// The interpolation form of the issue (§18.5: a hole renders via ToString).
const char* kShowInterp =
    "  public static method Show(o: Object) Console.WriteLine($\"[{o}]\"); end Show; ";

// A 200-iteration loop around `body`.
std::string loop200(const std::string& body) {
    return "var i := 0; while i < 200 do " + body + " i := i + 1; end while;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §2.1 / §3.1 — the issue's repro, looped: a StringBuilder through an
// Object parameter prints its text ("q"), 200 times, with no leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           writeline_object_param_stringbuilder_prints_content_stdlib_2_1_3_1) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("var sb := new StringBuilder(); "
                                       "_ := sb.Append(\"q\"); Show(sb);"))),
        rep("q\n", 200));
}

// §18.5 — the interpolation form of the repro.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           interpolation_object_param_stringbuilder_prints_content_18_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShowInterp, loop200("var sb := new StringBuilder(); "
                                             "_ := sb.Append(\"q\"); Show(sb);"))),
        rep("[q]\n", 200));
}

// §8.5 — `Array<int>` has no ToString of its own: the qualified name, with the
// type argument (stdlib §2.13 rule 3), through the Object parameter.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           writeline_object_param_array_int_prints_qualified_name_8_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("var a := new Array<int>(); a.Add(1); Show(a);"))),
        rep("System.Collections.Generic.Array<int>\n", 200));
}

// §8.5 — an array LITERAL (a different construction site from `new`) carries
// the same tag: the qualified name with its type argument, through the Object
// parameter. (The statically typed `Console.WriteLine(a)` renders the same text
// — emitVtablelessBuiltinToString shares the renderer — but is pinned by the
// 6753 rows, not here; #6889 is the leak that shape has on the base.)
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           writeline_object_param_array_literal_prints_qualified_name_8_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("var a: Array<string> := [\"x\", \"y\"]; Show(a);"))),
        rep("System.Collections.Generic.Array<string>\n", 200));
}

// §8.5 / stdlib §6.2 — `Mutex` renders Object's default: its qualified name.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           writeline_object_param_mutex_prints_qualified_name_8_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("var mx := new Mutex(); Show(mx);"))),
        rep("System.Threading.Mutex\n", 200));
}

// Control: a real string through the same parameter is still a string.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           writeline_object_param_string_control_8_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kShow, loop200("var s := \"hello\"; Show(s);"))),
                    rep("hello\n", 200));
}

// Control: a user class with a `ToString` override still dispatches to it.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           writeline_object_param_user_class_control_8_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("var d := new D(); Show(d);"),
                        "public class D public override method ToString(): string "
                        "=> \"dd\"; end D; ")),
        rep("dd\n", 200));
}

// ---------------------------------------------------------------------------
// §8.3 — `Equals` through an Object parameter is reference identity for a
// built-in with no override: a value equals itself and not another.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           equals_through_object_param_is_identity_8_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(
            "  public static method Cmp(o: Object, p: Object) "
            "    Console.WriteLine(o.Equals(p)); Console.WriteLine(o.Equals(o)); "
            "  end Cmp; ",
            "var sb := new StringBuilder(); var sb2 := new StringBuilder(); "
            "Cmp(sb, sb2); "
            "var xs := new Array<int>(); Cmp(xs, sb); "
            "var mx := new Mutex(); Cmp(mx, xs); Cmp(mx, mx);")),
        std::string("False\nTrue\nFalse\nTrue\nFalse\nTrue\nTrue\nTrue\n"));
}

// §8.3 — `GetHashCode` through an Object parameter is stable for one value and
// differs between two distinct values.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           gethashcode_through_object_param_stable_and_distinct_8_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(
            "  public static method Hash(o: Object, p: Object) "
            "    Console.WriteLine(o.GetHashCode() = o.GetHashCode()); "
            "    Console.WriteLine(o.GetHashCode() <> p.GetHashCode()); "
            "  end Hash; ",
            "var sb := new StringBuilder(); var sb2 := new StringBuilder(); "
            "Hash(sb, sb2); "
            "var xs := new Array<int>(); Hash(xs, sb); "
            "var mx := new Mutex(); Hash(mx, xs);")),
        std::string("True\nTrue\nTrue\nTrue\nTrue\nTrue\n"));
}

// ---------------------------------------------------------------------------
// §12.7 — `is` / `as` through an Object parameter answer by the value's
// runtime type: each of the three built-ins matches exactly its own type, and a
// string matches none.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           is_as_through_object_param_matches_own_type_only_12_7) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(
            "  public static method Kind(o: Object) "
            "    if o is StringBuilder then Console.WriteLine(\"sb\"); "
            "    else Console.WriteLine(\"not-sb\"); end if; "
            "    var a := o as Array<int>; "
            "    if a <> null then Console.WriteLine(\"arr\"); "
            "    else Console.WriteLine(\"not-arr\"); end if; "
            "    if o is Mutex then Console.WriteLine(\"mx\"); "
            "    else Console.WriteLine(\"not-mx\"); end if; "
            "  end Kind; ",
            "var sb := new StringBuilder(); _ := sb.Append(\"q\"); Kind(sb); "
            "var xs := new Array<int>(); xs.Add(1); Kind(xs); "
            "var mx := new Mutex(); Kind(mx); "
            "Kind(\"str\");")),
        std::string("sb\nnot-arr\nnot-mx\n"
                    "not-sb\narr\nnot-mx\n"
                    "not-sb\nnot-arr\nmx\n"
                    "not-sb\nnot-arr\nnot-mx\n"));
}

// §12.7 — the type ARGUMENT is part of the runtime type: an `Array<int>` is not
// an `Array<string>`, and the `as` result is usable as the target type.
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           is_as_array_through_object_param_distinguishes_type_argument_12_7) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(
            "  public static method Probe(o: Object) "
            "    Console.WriteLine(o is Array<int>); "
            "    Console.WriteLine(o is Array<string>); "
            "    var a := o as Array<int>; "
            "    if a <> null then Console.WriteLine(a.Count); end if; "
            "  end Probe; ",
            "var xs := new Array<int>(); xs.Add(1); xs.Add(2); Probe(xs); "
            "var ys := new Array<string>(); ys.Add(\"a\"); Probe(ys);")),
        std::string("True\nFalse\n2\nFalse\nTrue\n"));
}

// §19.3 — a named pattern over an Object scrutinee is the same runtime test:
// a StringBuilder and a Mutex each select their own arm; an array and a user
// class instance select neither. (A `string =>` arm is deliberately absent: on
// the base an Object scrutinee's `string` pattern in a case EXPRESSION is
// unconditionally true — a separate defect, #6888.)
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           case_named_pattern_through_object_param_19_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(
            "  public static method Tag(o: Object): int "
            "    return (case o of StringBuilder => 1, Mutex => 2, _ => 9); "
            "  end Tag; ",
            "var sb := new StringBuilder(); var mx := new Mutex(); "
            "var xs := new Array<int>(); var d := new D(); "
            "Console.WriteLine(Tag(sb)); Console.WriteLine(Tag(mx)); "
            "Console.WriteLine(Tag(xs)); Console.WriteLine(Tag(d));",
            "public class D end D; ")),
        std::string("1\n2\n9\n9\n"));
}

// ---------------------------------------------------------------------------
// §28.3 / §8.1 — disposal THROUGH `Object`: `using var o: Object := new X()`
// disposes the value by its runtime type (the builder's parts, the array's
// storage, the mutex's core), 200 times, with no leak and no double free.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           using_object_stringbuilder_disposes_by_runtime_type_28_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("using var o: Object := new StringBuilder(); Show(o);"))),
        rep("\n", 200));
}

BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           using_object_array_disposes_by_runtime_type_28_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("using var o: Object := new Array<int>(); Show(o);"))),
        rep("System.Collections.Generic.Array<int>\n", 200));
}

BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           using_object_mutex_disposes_by_runtime_type_28_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(kShow, loop200("using var o: Object := new Mutex(); Show(o);"))),
        rep("System.Threading.Mutex\n", 200));
}

// stdlib §6.2 (#706) — a Lock taken on a Mutex that is then disposed through
// `Object` keeps the mutex core alive until the Lock is released: no
// use-after-free, no double free. The mutex is MOVED into an `Object` local
// that is disposed at scope end while the Lock is still held: the Lock is
// declared first, so the reverse-order scope end (§28.10) disposes the mutex
// through `Object` before it releases the Lock. MEASURED on 279931c7 (the
// base), ASan armed: SEGV (a read of address 0x1 in main) before `done` is
// reached; on the fix: `done`, exit 0, no leak. #6990: `Dispose()` is not a
// member of `Object` (§8.7), so the explicit `o.Dispose()` became `using var`
// (§28.10). The discard `_ := o;` (§17.13) is not used: on this shape it leaks
// the mutex core, on base as well (#7026).
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           mutex_disposed_through_object_while_locked_outlives_lock_6_2) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("", loop200("var mx := new Mutex(); using var lk := mx.Acquire(); "
                                    "using var o: Object := move mx; ") +
                            " Console.WriteLine(\"done\");")),
        std::string("done\n"));
}

// ---------------------------------------------------------------------------
// stdlib §4.16 / §4.15 — `SequenceEqual` through the `IReadOnlySpan<T>` adapter
// (§4.16 retires the name, which the compiler still admits; the rule is §4.15's:
// equal length AND equal slots at every position). The adapter's thunk reads the
// span source's element buffer; after #6776 moved the buffer pointer behind the
// runtime type tag, a thunk still reading word 0 compares the two arrays'
// (identical) vtable words and reports every equal-length pair EQUAL. MEASURED
// on 0179eed5 (the first #6776 commit): `True True`; on the fix: `False True`.
//
// Plain oracle (`runAot`), not the checked one: the adapter box minted for a
// span argument at an `IReadOnlySpan<T>` parameter leaks 32 B per call on the
// base and on the fix alike (#6896, filed, independent of this defect), which
// would fail the leak oracle regardless of what `SequenceEqual` answers. Upgrade
// to `checkedOut` when #6896 lands.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           sequenceequal_through_ireadonlyspan_int_compares_content_4_15) {
    BEZEL_EXPECT_EQ(
        plainOut(prog(
            "  public static method Eq(a: IReadOnlySpan<int>, b: IReadOnlySpan<int>): bool "
            "    return a.SequenceEqual(b); end Eq; ",
            "using var a1 := new Array<int>(3); a1.Add(1); a1.Add(2); a1.Add(3); "
            "using var a2 := new Array<int>(3); a2.Add(1); a2.Add(2); a2.Add(4); "
            "Console.WriteLine(Eq(a1.AsReadOnlySpan(), a2.AsReadOnlySpan())); "
            "Console.WriteLine(Eq(a1.AsReadOnlySpan(), a1.AsReadOnlySpan()));")),
        std::string("False\nTrue\n"));
}

BEZEL_TEST(codegen_object_param_vtableless_builtin_6776,
           sequenceequal_through_ireadonlyspan_long_40_elements_compares_content_4_15) {
    BEZEL_EXPECT_EQ(
        plainOut(prog(
            "  public static method Eq(a: IReadOnlySpan<long>, b: IReadOnlySpan<long>): bool "
            "    return a.SequenceEqual(b); end Eq; ",
            "using var a1 := new Array<long>(40); using var a2 := new Array<long>(40); "
            "var i := 0; while i < 40 do a1.Add(i); a2.Add(i * 2); i := i + 1; end while; "
            "Console.WriteLine(Eq(a1.AsReadOnlySpan(), a2.AsReadOnlySpan())); "
            "Console.WriteLine(Eq(a1.AsReadOnlySpan(), a1.AsReadOnlySpan()));")),
        std::string("False\nTrue\n"));
}
