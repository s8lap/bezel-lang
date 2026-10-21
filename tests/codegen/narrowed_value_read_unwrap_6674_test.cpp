// fix #6674 (§7.4, §5.1, §22.6, v0.339): a read of a nullable VALUE-type
// binding that §7.4 has narrowed non-null, at a position whose target is the
// non-nullable `T`, must deliver the `T` payload, not the `{i8 hasValue; T}`
// carrier the binding is stored in.
//
//   §22.6 (v0.339) "The argument's static type is its type at the call: a read
//   of a binding §7.4 has narrowed non-null is of its non-nullable type, as
//   §7.11 reads it".
//   §7.4 (v0.339) "from a declaration whose initializer is such an `expr` —
//   `var x: T? := expr;` or `const x: T? := expr;` — which is the declaration's
//   assignment".
//   §5.1 (v0.339) "`S?` implicitly converts to `T?` whenever `S` implicitly
//   converts to `T`" — and never `T?` to `T`: only the narrowing makes these
//   programs well-typed.
//
// Before the fix, sema admitted the read (checkAssignable treats the narrowed
// `from` as non-nullable) but codegen passed the carrier on at every lowering
// site that did not repeat the unwrap itself: the Dictionary key stored the
// tag byte (`d[k] := 1` then `d.ContainsKey(5)` was False and `d[5]` threw
// KeyNotFoundException), a record `new RT(k)` copied the `{i8, int}` into an
// `int` component (ASan stack-buffer-overflow), `Array.Add`/`Contains`/
// indexer store/literal element, `Set.Add`/`Contains`, `in`, a generic
// record's `new Box<int>(k)` read garbage or the tag, and a function-value
// call `g(k)` and `k.Equals(5)` failed IR verification. Measured on e5887ed8
// (the base) for every row below; see the commit body.
//
// Each matrix row compiles ONE program in which every position reads a fresh
// narrowed binding in its own method, and asserts the exact stdout of the
// non-nullable control program (`var k: T := LIT;`, same positions). The
// spellings are the four §7.4 establishing forms reachable here: the
// declaration (#6223), `const`, an assignment, and an early-exit null test.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

struct Ty {
    const char* t;      // the Bezel type
    const char* lit;    // the narrowed value
    const char* lit2;   // a different value of the same type
    bool numeric;       // `+` / `<` rows apply
    bool enumFmt;       // print through int(...) (enum ToString is the type name)
};

const Ty kInt{"int", "5", "9", true, false};
const Ty kLong{"long", "5", "9", true, false};
const Ty kByte{"byte", "7", "byte(9)", true, false};
const Ty kDouble{"double", "2.5", "9.0", true, false};
const Ty kBool{"bool", "false", "true", false, false};
const Ty kChar{"char", "'q'", "'z'", false, false};
const Ty kDecimal{"decimal", "1.5m", "9m", true, false};
const Ty kEnum{"Color", "Color.Blue", "Color.Red", false, true};
const Ty kRecord{"Pt", "new Pt(1, 2)", "new Pt(8, 9)", false, false};

// The §7.4 establishing spellings. `return;` is rewritten for the methods that
// return a value or assign an `out`.
const char* kDecl = "var k: T? := LIT;";
const char* kConst = "const k: T? := LIT;";
const char* kAsg = "var k: T? := null; k := LIT;";
const char* kTest = "var k: T? := P.Mk(); if k = null then return; end;";

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos;
         p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
}

struct Pos {
    const char* name;
    const char* body;
    bool numericOnly;
};

// Positions whose pre-fix symptom was a SILENT wrong value (or an ASan fault).
const Pos kValuePositions[] = {
    {"call", "Console.WriteLine(P.Show(k));", false},
    {"ctor_class", "using var a := new A(k); Console.WriteLine(FMT{a._v});", false},
    {"ctor_record", "const r := new RT(k); Console.WriteLine(FMT{r.a});", false},
    {"dict_key", "var d := new Dictionary<T,int>(); d[k] := 1; Console.WriteLine(d.ContainsKey(LIT));", false},
    {"dict_add", "var d := new Dictionary<T,int>(); d.Add(k, 1); Console.WriteLine(d.ContainsKey(LIT));", false},
    {"dict_cont", "var d := new Dictionary<T,int>(); d[LIT] := 1; Console.WriteLine(d.ContainsKey(k));", false},
    {"dict_val", "var d := new Dictionary<int,T>(); d[0] := k; const w: T := d[0]; Console.WriteLine(FMT{w});", false},
    {"set_add", "var s := new Set<T>(); s.Add(k); Console.WriteLine(s.Contains(LIT));", false},
    {"set_cont", "var s := new Set<T>(); s.Add(LIT); Console.WriteLine(s.Contains(k));", false},
    {"in", "var l: Array<T> := [LIT]; Console.WriteLine(k in l);", false},
    {"arr_add", "var l := new Array<T>(); l.Add(k); const w: T := l[0]; Console.WriteLine(FMT{w});", false},
    {"arr_cont", "var l: Array<T> := [LIT]; Console.WriteLine(l.Contains(k));", false},
    {"arr_store", "var l: Array<T> := [LIT2]; l[0] := k; const w: T := l[0]; Console.WriteLine(FMT{w});", false},
    {"arr_lit", "var l: Array<T> := [k]; const w: T := l[0]; Console.WriteLine(FMT{w});", false},
    {"field_store", "using var a := new A(LIT2); a._v := k; Console.WriteLine(FMT{a._v});", false},
    {"prop_store", "using var a := new A(LIT2); a.Prop := k; Console.WriteLine(FMT{a._v});", false},
    {"eq", "Console.WriteLine(k = LIT);", false},
    {"gen_infer", "const w := P.Id(k); Console.WriteLine(FMT{w});", false},
    {"gen_explicit", "const w: T := P.Id<T>(k); Console.WriteLine(FMT{w});", false},
    {"gen_box", "const b := new Box<T>(k); Console.WriteLine(FMT{b.V});", false},
    {"tostring", "Console.WriteLine(k.ToString());", false},
    {"local_init", "var j: T := k; Console.WriteLine(FMT{j});", false},
    {"assign", "var j: T := LIT2; j := k; Console.WriteLine(FMT{j});", false},
    {"interp", "Console.WriteLine($\"{k}\");", false},
    {"add", "const w: T := k + LIT; Console.WriteLine(FMT{w});", true},
    {"lt", "Console.WriteLine(k < LIT2);", true},
};

// Positions whose pre-fix symptom was an IR-verification abort of bezelc.
const Pos kIcePositions[] = {
    {"lambda_arg", "const g: (T) -> string := lambda(x: T) => $\"{x}\"; Console.WriteLine(g(k));", false},
    {"equals", "Console.WriteLine(k.Equals(LIT));", false},
};

std::string fmt(const Ty& ty, const std::string& e) {
    return ty.enumFmt ? "int(" + e + ")" : "$\"{" + e + "}\"";
}

// FMT{e} -> how the row prints a T-typed expression `e`.
void expandFmt(std::string& s, const Ty& ty) {
    for (size_t p = s.find("FMT{"); p != std::string::npos; p = s.find("FMT{")) {
        size_t close = s.find('}', p);
        std::string e = s.substr(p + 4, close - (p + 4));
        s.replace(p, close + 1 - p, fmt(ty, e));
    }
}

template <size_t N>
std::string program(const Ty& ty, const char* spelling, const Pos (&positions)[N],
                    bool withMethodSites) {
    std::string methods, calls;
    for (const Pos& p : positions) {
        if (p.numericOnly && !ty.numeric) continue;
        // `byte + byte` is an `int` (§6.2), not a `byte`: the control rejects it.
        if (std::string(p.name) == "add" && std::string(ty.t) == "byte") continue;
        methods += std::string("  public static method R_") + p.name + "()\n    DECL\n    " +
                   p.body + "\n  end R_" + p.name + ";\n";
        calls += std::string("    P.R_") + p.name + "();\n";
    }
    if (withMethodSites) {
        // return / `out` / `ref` stores of the narrowed read.
        calls += "    Console.WriteLine(FMT{P.Ret()});\n"
                 "    P.O(out var j1); Console.WriteLine(FMT{j1});\n"
                 "    var j2: T := LIT2; P.Rf(ref j2); Console.WriteLine(FMT{j2});\n";
    }
    std::string src =
        "module M;\n"
        "public enum Color Red, Green, Blue; end Color;\n"
        "public record Pt(X: int, Y: int);\n"
        "public record RT(a: T);\n"
        "public record Box<U>(V: U);\n"
        "public class A\n"
        "  public constructor(v: T) _v := v; end constructor;\n"
        "  public _v: T;\n"
        "  public property Prop: T\n"
        "    get => _v;\n"
        "    set => _v := value;\n"
        "  end Prop;\n"
        "end A;\n"
        "public class P\n"
        "  public static method Show(x: T): string => $\"{x}\";\n"
        "  public static method Id<U>(x: U): U => x;\n"
        "  public static method Mk(): T? => LIT;\n"
        "  public static method Ret(): T\n    DECL2\n    return k;\n  end Ret;\n"
        "  public static method O(out o: T)\n    DECL3\n    o := k;\n  end O;\n"
        "  public static method Rf(ref o: T)\n    DECL3\n    o := k;\n  end Rf;\n" +
        methods +
        "  public static method Main(): int\n" + calls +
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    std::string d = spelling, d2 = spelling, d3 = spelling;
    replaceAll(d2, "return;", "return LIT2;");
    replaceAll(d3, "return;", "o := LIT2; return;");
    replaceAll(src, "DECL3", d3);
    replaceAll(src, "DECL2", d2);
    replaceAll(src, "DECL", d);
    expandFmt(src, ty);
    replaceAll(src, "LIT2", ty.lit2);
    replaceAll(src, "LIT", ty.lit);
    const std::string t = ty.t;
    replaceAll(src, "<T>", "<" + t + ">");
    replaceAll(src, "<T,", "<" + t + ",");
    replaceAll(src, ",T>", "," + t + ">");
    replaceAll(src, ": T", ": " + t);
    replaceAll(src, "T?", t + "?");
    replaceAll(src, "(T) ->", "(" + t + ") ->");
    return src;
}

// Expected stdout of the value-position program: the control (`var k: T :=
// LIT;`) prints `v` for a value row and True for a membership/equality row.
std::string expectedValue(const Ty& ty, const std::string& v) {
    std::string out;
    for (const Pos& p : kValuePositions) {
        if (p.numericOnly && !ty.numeric) continue;
        const std::string n = p.name;
        if (n == "add" && std::string(ty.t) == "byte") continue;
        if (n == "dict_key" || n == "dict_add" || n == "dict_cont" ||
            n == "set_add" || n == "set_cont" || n == "in" || n == "arr_cont" ||
            n == "eq" || n == "lt")
            out += "True\n";
        else if (n == "add")
            out += std::string(ty.t) == "int" || std::string(ty.t) == "long" ? "10\n"
                   : std::string(ty.t) == "double" ? "5\n" : "3\n";
        else if ((n == "call" || n == "tostring" || n == "interp") && ty.enumFmt)
            out += "M.Color\n";   // ToString of an enum renders its type name
        else
            out += v + "\n";
    }
    for (int i = 0; i < 3; ++i) out += v + "\n";   // Ret, out, ref
    return out;
}

std::string runOut(const std::string& src, bool checked = false) {
    auto r = checked ? runAotChecked(src) : runAot(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

void checkValue(const Ty& ty, const char* spelling, const std::string& v,
                bool checked = false) {
    BEZEL_EXPECT_EQ(runOut(program(ty, spelling, kValuePositions, true), checked),
                    expectedValue(ty, v) + "exit=0");
}

void checkIce(const Ty& ty, const char* spelling, const std::string& v) {
    // lambda_arg prints through `$"{x}"`, so an enum shows its type name.
    BEZEL_EXPECT_EQ(runOut(program(ty, spelling, kIcePositions, false)),
                    (ty.enumFmt ? std::string("M.Color") : v) + "\nTrue\nexit=0");
}

}  // namespace

// ---- the issue's own repros (the #6223 reviewer's probes, v_*.bzl) ---------

// v_dict_idx.decl: the key stored the tag byte, so `d[5]` threw
// KeyNotFoundException (exit 70, "Unhandled exception").
BEZEL_TEST(codegen_narrowed_value_read_6674, issue_dictionary_key_decl_spelling) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var i: int? := 5;\n"
        "    var d := new Dictionary<int,int>();\n"
        "    d[i] := i;\n"
        "    Console.WriteLine(d[5]);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("5\nexit=0"));
}

// v_dict_idx.asg: the base already reached it through the assignment spelling;
// it printed Count 1, key 1 (the tag), then threw.
BEZEL_TEST(codegen_narrowed_value_read_6674, issue_dictionary_key_assignment_spelling) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var i: int? := null; i := 5;\n"
        "    var d := new Dictionary<int,int>();\n"
        "    d[i] := i;\n"
        "    Console.WriteLine(d.Count);\n"
        "    for var k in d.Keys do Console.WriteLine(k); end;\n"
        "    Console.WriteLine(d[5]);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("1\n5\n5\nexit=0"));
}

// v_rec_ctor.decl: an ASan stack-buffer-overflow at the record constructor
// (the `{i8, int}` carrier copied into the `int` component); without ASan it
// printed 1. On the ASan oracle.
BEZEL_TEST(codegen_narrowed_value_read_6674, issue_record_ctor_arg_checked) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record R(a: int);\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var i: int? := 5;\n"
        "    const r := new R(i);\n"
        "    Console.WriteLine(r.a);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n", /*checked=*/true),
        std::string("5\nexit=0"));
}

// v_funcval.decl: `f(i)` through a `(int) -> int` function value failed IR
// verification.
BEZEL_TEST(codegen_narrowed_value_read_6674, issue_function_value_arg) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var i: int? := 5;\n"
        "    const f: (int) -> int := lambda(x: int) => x + 1;\n"
        "    Console.WriteLine(f(i));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("6\nexit=0"));
}

// #2625 item 1 (the half-fixed `string.Substring(start, length)` and
// `string.Remove(start, count)` second arguments): both are GATED in sema
// (checkBuiltinStringCallArgs' "the argument to `string.Substring` is
// nullable" reject), so sema's checkAssignable publishes the narrowed read and
// the shared unwrap reaches these builtin lowerings too. Pre-fix: IR
// verification failed ("SExt only operates on integer"). #2625's repro shape.
BEZEL_TEST(codegen_narrowed_value_read_6674, gated_builtin_string_int_args_2625) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public class P\n"
        "  private static method N(f: bool, k: int): int? if f then return k; end; return null; end N;\n"
        "  public static method Main(): int\n"
        "    const s := \"hello\";\n"
        "    var n: int? := P.N(true, 1); if n = null then return 0; end;\n"
        "    Console.WriteLine(s.Substring(0, n));\n"
        "    Console.WriteLine(s.Remove(1, n));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("h\nhllo\nexit=0"));
}

// ---- the position x type x spelling matrix -------------------------------

BEZEL_TEST(codegen_narrowed_value_read_6674, int_decl_positions) { checkValue(kInt, kDecl, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, int_const_positions) { checkValue(kInt, kConst, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, int_assignment_positions) { checkValue(kInt, kAsg, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, int_null_test_positions) { checkValue(kInt, kTest, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, long_decl_positions) { checkValue(kLong, kDecl, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, long_null_test_positions) { checkValue(kLong, kTest, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, byte_decl_positions) { checkValue(kByte, kDecl, "7"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, byte_null_test_positions) { checkValue(kByte, kTest, "7"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, double_decl_positions) { checkValue(kDouble, kDecl, "2.5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, double_null_test_positions) { checkValue(kDouble, kTest, "2.5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, bool_decl_positions) { checkValue(kBool, kDecl, "False"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, bool_null_test_positions) { checkValue(kBool, kTest, "False"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, char_decl_positions) { checkValue(kChar, kDecl, "q"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, char_null_test_positions) { checkValue(kChar, kTest, "q"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, decimal_decl_positions) { checkValue(kDecimal, kDecl, "1.5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, decimal_null_test_positions) { checkValue(kDecimal, kTest, "1.5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, enum_decl_positions) { checkValue(kEnum, kDecl, "2"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, enum_null_test_positions) { checkValue(kEnum, kTest, "2"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, record_decl_positions) { checkValue(kRecord, kDecl, "Pt(X: 1, Y: 2)"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, record_null_test_positions) { checkValue(kRecord, kTest, "Pt(X: 1, Y: 2)"); }

// The same programs under generated-code ASan: the record row is where the
// pre-fix carrier overran the component (stack-buffer-overflow).
BEZEL_TEST(codegen_narrowed_value_read_6674, record_decl_positions_checked) {
    checkValue(kRecord, kDecl, "Pt(X: 1, Y: 2)", /*checked=*/true);
}
BEZEL_TEST(codegen_narrowed_value_read_6674, int_assignment_positions_checked) {
    checkValue(kInt, kAsg, "5", /*checked=*/true);
}

BEZEL_TEST(codegen_narrowed_value_read_6674, int_decl_ice_positions) { checkIce(kInt, kDecl, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, int_null_test_ice_positions) { checkIce(kInt, kTest, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, long_decl_ice_positions) { checkIce(kLong, kDecl, "5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, byte_decl_ice_positions) { checkIce(kByte, kDecl, "7"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, double_decl_ice_positions) { checkIce(kDouble, kDecl, "2.5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, bool_decl_ice_positions) { checkIce(kBool, kDecl, "False"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, char_decl_ice_positions) { checkIce(kChar, kDecl, "q"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, decimal_decl_ice_positions) { checkIce(kDecimal, kDecl, "1.5"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, enum_decl_ice_positions) { checkIce(kEnum, kDecl, "2"); }
BEZEL_TEST(codegen_narrowed_value_read_6674, record_decl_ice_positions) { checkIce(kRecord, kDecl, "Pt(X: 1, Y: 2)"); }

// ---- reject controls: the pairing sema gate is unchanged ------------------
// The unwrap is sound only because sema rejects the UN-narrowed read at these
// positions (§5.1: no `T?` -> `T`). Pin the diagnostics so a later change that
// drops a gate cannot turn the unwrap into a read of a possibly-null payload.

namespace {
// True when the program is rejected and an error diagnostic
// carries `needle` (runAot reports the compiler's stderr as one Error).
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}
std::string unNarrowed(const std::string& body) {
    return "module M;\n"
           "public record R(a: int);\n"
           "public class P\n"
           "  public static method Mk(): int? => 5;\n"
           "  public static method Main(): int\n"
           "    var k: int? := P.Mk();\n    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}
}  // namespace

BEZEL_TEST(codegen_narrowed_value_read_6674, reject_unnarrowed_dictionary_key) {
    BEZEL_EXPECT_TRUE(rejectsWith(unNarrowed(
        "var d := new Dictionary<int,int>(); d[k] := 1;"),
        "the index argument to `Dictionary[key]` is nullable but "
                    "the target type is non-nullable (§7)"));
}
BEZEL_TEST(codegen_narrowed_value_read_6674, reject_unnarrowed_record_ctor_arg) {
    BEZEL_EXPECT_TRUE(rejectsWith(unNarrowed(
        "const r := new R(k); Console.WriteLine(r.a);"),
        "the argument `a` to `new R` is nullable but the target "
                    "type is non-nullable (§7)"));
}
BEZEL_TEST(codegen_narrowed_value_read_6674, reject_unnarrowed_in_operand) {
    BEZEL_EXPECT_TRUE(rejectsWith(unNarrowed(
        "var l: Array<int> := [5]; Console.WriteLine(k in l);"),
        "the left operand of `in` over `Array` is nullable but the "
                    "target type is non-nullable (§7)"));
}
