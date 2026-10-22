// fix #6924 (stdlib §4.19, spec §28.11 / §28.9, v0.340):
// `FrozenArray<string>.From(items: Array<string>)` is "the copying form, the
// only construction form for T = string". A FrozenArray<string> co-owns a +1
// per element and releases it at its own free. `tryLowerFrozenArrayCall`
// called the shared retain helper with the FrozenArray result type, which that
// helper does not recognise (it accepts only Array/Stack/Queue heads), so the
// copy took NO retain. For a BORROWED source (a local, a parameter, a field)
// the source's owner and the copy then released the same +1. MEASURED on
// c0a1928b, generated-code ASan:
//   * the issue's repro (`using var a` local source): heap-use-after-free;
//   * a parameter source and an `owned` field source: "refcount over-release
//     (string)" abort, rc 134, after one line of output;
//   * a FrozenArray nested in an OwningArray (released through the #6923
//     element disposer): heap-use-after-free.
// An OWNED source (a literal, a call result, `move`) is freed by `From` with a
// NULL element disposer, which hands its +1 per string to the copy; those
// shapes were clean and must stay leak-free — the retain is keyed on the
// source being borrowed. Rows 5-7 pin that (LSan armed: an extra retain is a
// leak and flips `ran`).
//
// Every row loops 100x on `runAotChecked` and asserts the stdout, which reads
// an element of the copy (and of the source, where it is still live).

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotChecked;

namespace {

std::string repeatLine(const std::string& line, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += line + "\n";
    return s;
}

void expectClean(const JitResult& r, const std::string& expectedStdout) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

constexpr const char* kLib =
    "public class H\n"
    "  public owned Xs: Array<string>;\n"
    "  public constructor() Xs := new Array<string>(); Xs.Add(\"f\" + \"1\"); "
    "Xs.Add(\"f\" + \"2\"); end constructor;\n"
    "  public method Freeze(): owned FrozenArray<string>\n"
    "    return FrozenArray<string>.From(Xs);\n"
    "  end Freeze;\n"
    "end H;\n"
    "public class M\n"
    "  public static method MkArr(): owned Array<string>\n"
    "    var a := new Array<string>(); a.Add(\"a\" + \"x\"); a.Add(\"b\" + \"y\"); "
    "a.Add(\"c\" + \"z\");\n"
    "    return a;\n"
    "  end MkArr;\n"
    "  public static method Mk(): owned FrozenArray<string>\n"
    "    using var a := new Array<string>();\n"
    "    a.Add(\"a\" + \"x\"); a.Add(\"b\" + \"y\"); a.Add(\"c\" + \"z\");\n"
    "    var v := FrozenArray<string>.From(a);\n"
    "    return v;\n"
    "  end Mk;\n"
    "  public static method FromParam(a: Array<string>): owned FrozenArray<string>\n"
    "    return FrozenArray<string>.From(a);\n"
    "  end FromParam;\n"
    "end M;\n";

std::string mainLoop(const std::string& body) {
    return "class C\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    while i < 100 do\n"
           "      " + body + "\n"
           "      i += 1;\n"
           "    end;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

std::string program(const std::string& body) {
    return std::string("module Loc;\n") + kLib + mainLoop(body);
}

}  // namespace

// Row 1 — the issue's repro verbatim: a `using var` local source.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           local_source_issue_repro_spec_stdlib_4_19) {
    expectClean(runAotChecked(program(
        "using var x := M.Mk(); Console.WriteLine($\"{x.Length} {x[1]}\");")),
        repeatLine("3 by", 100));
}

// Row 2 — a parameter source: both the copy and the caller's array are read.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           parameter_source_spec_stdlib_4_19) {
    expectClean(runAotChecked(program(
        "using var a := M.MkArr(); using var x := M.FromParam(a); "
        "Console.WriteLine($\"{x.Length} {x[2]} {a[2]}\");")),
        repeatLine("3 cz cz", 100));
}

// Row 3 — an `owned` field source, read through the field after the copy.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           owned_field_source_spec_stdlib_4_19) {
    expectClean(runAotChecked(program(
        "using var h := new H(); using var x := h.Freeze(); "
        "Console.WriteLine($\"{x.Length} {x[1]} {h.Xs[1]}\");")),
        repeatLine("2 f2 f2", 100));
}

// Row 4 — the copy nested in an OwningArray, released there by the per-element
// disposer (§28.11 owned slot); one element from a borrowed local source.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           nested_in_owning_array_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var o := new OwningArray<FrozenArray<string>>(); o.Add(M.Mk()); "
        "using var a := M.MkArr(); o.Add(FrozenArray<string>.From(a)); "
        "Console.WriteLine($\"{o.Count} {o[0][1]} {o[1][2]}\");")),
        repeatLine("2 by cz", 100));
}

// Rows 5-7 — OWNED sources (control: clean before and after; LSan catches a
// retain the owned path must not take).
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           owned_literal_source_takes_no_extra_retain_spec_stdlib_4_19) {
    expectClean(runAotChecked(program(
        "using var x := FrozenArray<string>.From([\"a\" + \"x\", \"b\" + \"y\"]); "
        "Console.WriteLine($\"{x.Length} {x[1]}\");")),
        repeatLine("2 by", 100));
}

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           owned_call_result_source_takes_no_extra_retain_spec_stdlib_4_19) {
    expectClean(runAotChecked(program(
        "using var x := FrozenArray<string>.From(M.MkArr()); "
        "Console.WriteLine($\"{x.Length} {x[0]}\");")),
        repeatLine("3 ax", 100));
}

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           moved_source_takes_no_extra_retain_spec_stdlib_4_19) {
    expectClean(runAotChecked(program(
        "var a := M.MkArr(); using var x := FrozenArray<string>.From(move a); "
        "Console.WriteLine($\"{x.Length} {x[2]}\");")),
        repeatLine("3 cz", 100));
}

// Row 9 — disposal that does not know the static type: `o.Dispose()` through
// `Object` reads the element disposer from the array header. With the retain
// of rows 1-4 the copy co-owns its strings, so the header must name the string
// release (else one string per element leaks), for a borrowed source and an
// owned one alike.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           object_typed_dispose_releases_copied_strings_spec_28_11) {
    expectClean(runAotChecked(program(
        "var x := M.Mk(); var o: Object := move x; o.Dispose(); "
        "var y := FrozenArray<string>.From(M.MkArr()); var p: Object := move y; "
        "p.Dispose(); Console.WriteLine(\"d\");")),
        repeatLine("d", 100));
}

// Row 10 — an `owned` FrozenArray<string> FIELD: released at the holder's
// disposal and, when overwritten, for the displaced value (the issue's
// `fieldown` / `refield` cells). Mixes borrowed-source and owned-source copies.
// MEASURED on c0a1928b: heap-use-after-free. With the call-site retain alone
// the field paths passed a NULL element disposer (they recognised only the
// Array/Stack/Queue/LinkedList heads): 10,500 B / 31,500 B of strings leaked.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           owned_field_holds_and_overwrites_copies_spec_28_11) {
    const std::string box =
        "class Box\n"
        "  private owned _s: FrozenArray<string>;\n"
        "  public constructor() _s := M.Mk(); end constructor;\n"
        "  public method Reset() _s := M.Mk(); end Reset;\n"
        "  public method ResetOwned() _s := FrozenArray<string>.From(M.MkArr()); "
        "end ResetOwned;\n"
        "  public method Show() Console.WriteLine($\"{_s.Length} {_s[1]}\"); end Show;\n"
        "end Box;\n";
    expectClean(runAotChecked(std::string("module Loc;\n") + kLib + box + mainLoop(
        "using var a := new Box(); a.Show(); "
        "using var b := new Box(); b.Reset(); b.ResetOwned(); b.Reset(); b.Show();")),
        repeatLine("3 by\n3 by", 100));
}

// Row 8 — cross-module: the copying factories live in an imported module.
BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           cross_module_borrowed_sources_spec_stdlib_4_19) {
    std::vector<ModuleSource> mods;
    mods.push_back({"LibX", std::string("module LibX;\n") + kLib});
    mods.push_back({"App",
        "module App;\nimport LibX;\n" + mainLoop(
            "using var x := M.Mk(); using var a := M.MkArr(); "
            "using var y := M.FromParam(a); using var h := new H(); "
            "using var z := h.Freeze(); "
            "Console.WriteLine($\"{x[1]} {y[2]} {a[0]} {z[0]}\");")});
    expectClean(bezel::test::runAotModulesChecked(std::move(mods), "App"),
                repeatLine("by cz ax f1", 100));
}

// ---- follow-up (reviewer F1): the same copy made INSIDE A GENERIC BODY. -----
// `resTy` there is FrozenArray<T>; deciding string-ness from the uninstantiated
// `T` skipped the retain/stamp while a non-generic holder's disposal (which sees
// FrozenArray<string>) released: "refcount over-release (string)" abort at
// d8d42847, clean at c0a1928b. The element is now resolved through the active
// substitution, so T = string behaves like the non-generic path.
namespace {
constexpr const char* kGenLib =
    "public class M\n"
    "  public static method MkArr(): owned Array<string>\n"
    "    var a := new Array<string>(); a.Add(\"a\" + \"x\"); a.Add(\"b\" + \"y\"); "
    "a.Add(\"c\" + \"z\");\n"
    "    return a;\n"
    "  end MkArr;\n"
    "end M;\n"
    "public class G\n"
    "  public static method Freeze<T>(a: Array<T>): owned FrozenArray<T>\n"
    "    return FrozenArray<T>.From(a);\n"
    "  end Freeze;\n"
    "  public static method FreezeOwned<T>(owned a: Array<T>): owned FrozenArray<T>\n"
    "    return FrozenArray<T>.From(move a);\n"
    "  end FreezeOwned;\n"
    "end G;\n"
    "public class Holder<T>\n"
    "  private owned _f: FrozenArray<T>;\n"
    "  public constructor(a: Array<T>) _f := FrozenArray<T>.From(a); end constructor;\n"
    "  public method Len(): int return _f.Length; end Len;\n"
    "end Holder;\n"
    "public class H3\n"
    "  private owned _f: FrozenArray<string>;\n"
    "  public constructor(a: Array<string>) _f := G.Freeze<string>(a); end constructor;\n"
    "  public method Len(): int return _f.Length; end Len;\n"
    "end H3;\n";

std::string genProgram(const std::string& body) {
    return std::string("module Loc;\n") + kGenLib + mainLoop(body);
}
}  // namespace

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           generic_fn_into_owned_field_spec_28_11) {
    expectClean(runAotChecked(genProgram(
        "using var a := M.MkArr(); using var h := new H3(a); "
        "Console.WriteLine($\"{h.Len()} {a[1]}\");")),
        repeatLine("3 by", 100));
}

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           generic_holder_owned_field_spec_28_11) {
    expectClean(runAotChecked(genProgram(
        "using var a := M.MkArr(); using var h := new Holder<string>(a); "
        "Console.WriteLine($\"{h.Len()} {a[1]}\");")),
        repeatLine("3 by", 100));
}

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           generic_fn_borrowed_local_spec_stdlib_4_19) {
    expectClean(runAotChecked(genProgram(
        "using var a := M.MkArr(); using var x := G.Freeze<string>(a); "
        "Console.WriteLine($\"{x.Length} {x[2]} {a[2]}\");")),
        repeatLine("3 cz cz", 100));
}

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           generic_fn_owned_source_takes_no_extra_retain_spec_stdlib_4_19) {
    expectClean(runAotChecked(genProgram(
        "using var x := G.FreezeOwned<string>(M.MkArr()); "
        "var b := M.MkArr(); using var y := G.FreezeOwned<string>(move b); "
        "Console.WriteLine($\"{x.Length} {x[0]} {y[2]}\");")),
        repeatLine("3 ax cz", 100));
}

BEZEL_TEST(frozen_array_from_borrowed_string_array_retain_6924,
           generic_fn_int_element_control_spec_stdlib_4_19) {
    expectClean(runAotChecked(genProgram(
        "using var a := new Array<int>(); a.Add(7); a.Add(8); "
        "using var x := G.Freeze<int>(a); using var h := new Holder<int>(a); "
        "Console.WriteLine($\"{x.Length} {x[1]} {h.Len()}\");")),
        repeatLine("2 8 2", 100));
}
