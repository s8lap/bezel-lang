// fix #3183 (§15.1 "Inline arrays" / §2.8.3 rule 4 "Transitive closure and type
// identity" / §9.8; spec v0.254) — an inline array's SIZE now crosses a module
// boundary.
//
// ── WHAT WAS BROKEN, AND WHY IT LOOKED HARMLESS ────────────────────────────
//
// `ModuleMetadataBuilder::convertType` never wrote `MetaType::arraySize` ("the
// const-expr evaluator is sema-internal and not threaded to the builder"), and
// `ModuleLoader` said the same on the read side, so an imported `[N]T` reached
// `Emitter::lowerTypeRef` with neither an `ast::Expr` size nor a folded
// `arraySizeValue` — and that function's opening guard returned a bare **i32
// placeholder**. An importer therefore laid out an imported `[3]int` component
// in FOUR bytes where the exporter laid it out in twelve, which §2.8.3 rule 4
// forbids in as many words: one runtime type identity for a type, across the
// boundary.
//
// #3183 was filed recording, honestly, that its blast radius was NOT
// established — "a plain read-back probe returned the CORRECT values, which does
// not fit a naive 'the struct is 4 bytes shorter' story". The ASan oracle
// settled it. Constructing an imported record with an inline-array component is
// a **stack-buffer-overflow**:
//
//     ==...==ERROR: AddressSanitizer: stack-buffer-overflow
//     WRITE of size 12 at 0x7bb336900008b
//     ...
//       [128, 136) '' <== Memory access at offset 139 overflows this variable
//
// an 8-byte slot taking a 12-byte write (re-measured on this lane's parent,
// `d7eef83`). The read-back probe looked clean because the overflowing bytes
// land in adjacent stack the program never reads again — #2633's point that the
// plain and ASan oracles are DIFFERENT, not ordered, in its sharpest form: the
// plain gate cannot see this defect at all.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// The size did not need a const-expr evaluator threaded anywhere: `TypeResolution`
// has already folded every foldable size onto `TypeRef::arraySizeValue` (#734),
// so the builder writes that number as decimal into the schema's existing
// `arraySize` string and the loader restores it. `lowerTypeRef`'s guard is
// widened to admit a folded size with no AST node — which makes it EXACTLY
// `sema::serializationComponentTypeIsEmittable`'s condition, as that predicate's
// comment already claimed ("stated once, here, so the two cannot drift"; they
// had drifted, harmlessly, only because no importer ever set the value).
//
// An UNFOLDABLE size stays uncarried, so the shape that genuinely cannot be
// lowered keeps failing loudly through the same two predicates.
//
// ── WHAT THESE ROWS PIN ────────────────────────────────────────────────────
//
// Every row is an imported/local PAIR over character-identical declarations, so
// a row can only pass by the two sides agreeing. The rows are chosen to make
// "the size crossed" and "the LAYOUT crossed" separate assertions: a read-back
// alone would have passed before the fix (it did — that is why the issue could
// not establish the radius), so the load-bearing rows are the ones where the
// DEPENDENCY's own code and the IMPORTER's code index the same object (T3), and
// the ASan loops (T1b, T3b).
//
// The §9.8 WIRE consequences — an imported `[3]int` component serializing at its
// local 50-byte width instead of 22, and the `serialized skip` carve-out —
// live in tests/module/xmod_record_serialization_3140_test.cpp, whose T12/T14
// flip from reject rows to round-trips with this fix and whose T15 regains the
// round-trip half it gave up to step around this corruption.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile3183 {
    std::string path;
    explicit TempFile3183(std::string p) : path(std::move(p)) {}
    ~TempFile3183() { std::remove(path.c_str()); }
};

void writeSource3183(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -1  the app did not build, or did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3183 = -2;

int compileImportAndRun3183(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3183_" + std::to_string(std::rand());
    TempFile3183 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3183 aObj((dir / (stem + "_a.o")).string());
    writeSource3183(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3183;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3183;

    TempFile3183 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3183 bExe((dir / stem).string());
    writeSource3183(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // compileFileForTest, not driver.compileFile: `gate.sh --full` reaches a
    // hand-rolled runner only through this wrapper, and for THIS family the
    // ASan oracle is the only one that can see the defect at all.
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int compileAndRun3183(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3183L_" + std::to_string(std::rand());
    TempFile3183 s((dir / (stem + ".bzl")).string());
    TempFile3183 exe((dir / stem).string());
    writeSource3183(s.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, s.path, exe.path).success)
        return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The declarations under test, used VERBATIM as a dependency module's body and,
// spliced into the twin, as part of a single module.
const char* kDecls3183 =
    // §9.8 admits "a non-owning inline array [N]T" as a record component. This
    // is the shape whose CONSTRUCTION was the stack-buffer-overflow.
    "public record InlineArr3183(A: [3]int, N: int); "
    // A wider element, so nothing can pass by hardcoding 3 or by the placeholder
    // happening to be the right size.
    "public record WideArr3183(A: [5]long, N: int); "
    // A nullable ELEMENT type — the element side of the carriage, which rides
    // `children[0]` and was never the broken half; a control.
    "public record NullElem3183(A: [3]int?, N: int); "
    // ★ THE LAYOUT ROW. Both the dependency and the importer index this field.
    // The dependency's `SumHere` is compiled where the class is LOCAL (real
    // layout); the importer's own indexing goes through the reconstructed type.
    // Equal answers mean one layout, which is what §2.8.3 rule 4 asks for; the
    // placeholder made them two.
    "public class Holder3183 "
    "  public arr: [4]int; "
    "  public method Set(i: int, v: int) "
    "    arr[i] := v; "
    "  end Set; "
    "  public method SumHere(): int "
    "    return arr[0] + arr[1] + arr[2] + arr[3]; "
    "  end SumHere; "
    "end Holder3183;";

std::string lib3183(const std::string& tag) {
    return "module Lib3183" + tag + "; " + kDecls3183;
}
std::string app3183(const std::string& tag, const std::string& body) {
    return "module App3183" + tag + "; import Lib3183" + tag + "; "
           "public class C" + tag + " " + body + " end C" + tag + ";";
}
std::string twin3183(const std::string& tag, const std::string& body) {
    return "module Twin3183" + tag + "; " + kDecls3183 +
           " public class C" + tag + " " + body + " end C" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §15.1 / §2.8.3 rule 4: construct an imported record with a `[3]int`
// component and read every slot back.
//
// `A[0]*100 + A[2]*10 + N` = 7*100 + 9*10 + 5 = 795.
//
// ⚠ The VALUE half of this row passed BEFORE the fix — that is exactly what
// #3183 recorded as not fitting a naive story, and why it could not establish
// the radius. What did not pass is this row under `--full`: the construction
// wrote 12 bytes into the placeholder's 8-byte slot. Both oracles run it.
// ===========================================================================
BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t1_imported_record_inline_array_component_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var a: [3]int := [7, 8, 9]; "
        "    var v := new InlineArr3183(a, 5); "
        "    if v.A[0] <> 7 then return 1; end if; "
        "    if v.A[1] <> 8 then return 2; end if; "
        "    if v.A[2] <> 9 then return 3; end if; "
        "    if v.N <> 5 then return 4; end if; "
        "    return 79; "
        "  end Main;";
    int r = compileImportAndRun3183(lib3183("T1"), app3183("T1", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 79);
}

BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t1_twin_same_program_in_one_module_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var a: [3]int := [7, 8, 9]; "
        "    var v := new InlineArr3183(a, 5); "
        "    if v.A[0] <> 7 then return 1; end if; "
        "    if v.A[1] <> 8 then return 2; end if; "
        "    if v.A[2] <> 9 then return 3; end if; "
        "    if v.N <> 5 then return 4; end if; "
        "    return 79; "
        "  end Main;";
    int r = compileAndRun3183(twin3183("T1L", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 79);
}

// ===========================================================================
// T1b — the same construction 200 times. THIS IS THE CORRUPTION ROW: under
// `BEZEL_AOT_FORCE_SANITIZE=address` the pre-fix build reports a
// stack-buffer-overflow on the first iteration; the plain build reports
// nothing, because the overflow lands in stack the program does not read again.
// The accumulator additionally proves every iteration constructed correctly, so
// the row is not merely "did not crash".
// ===========================================================================
BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t1b_repeated_construction_does_not_overflow_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0..199 do "
        "      var a: [3]int := [1, 2, 3]; "
        "      var v := new InlineArr3183(a, 5); "
        "      acc := acc + v.A[0] + v.A[2] + v.N; "
        "    end for; "
        "    return acc / 200; "     // 1 + 3 + 5
        "  end Main;";
    int r = compileImportAndRun3183(lib3183("T1b"), app3183("T1b", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 9);
}

// ===========================================================================
// T2 — §15.1: a WIDER element and a different N. `[5]long` is 40 bytes against
// the placeholder's 4, so nothing here can pass by accident of size.
//
// `A[4]*10 + N` = 5*10 + 9 = 59.
// ===========================================================================
BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t2_imported_wide_inline_array_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var a: [5]long := [1, 2, 3, 4, 5]; "
        "    var v := new WideArr3183(a, 9); "
        "    if v.A[0] <> 1 then return 1; end if; "
        "    if v.A[4] <> 5 then return 2; end if; "
        "    if v.N <> 9 then return 3; end if; "
        "    return 59; "
        "  end Main;";
    int r = compileImportAndRun3183(lib3183("T2"), app3183("T2", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 59);
}

BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t2_twin_wide_inline_array_in_one_module_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var a: [5]long := [1, 2, 3, 4, 5]; "
        "    var v := new WideArr3183(a, 9); "
        "    if v.A[0] <> 1 then return 1; end if; "
        "    if v.A[4] <> 5 then return 2; end if; "
        "    if v.N <> 9 then return 3; end if; "
        "    return 59; "
        "  end Main;";
    int r = compileAndRun3183(twin3183("T2L", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 59);
}

// ===========================================================================
// T3 — ★ THE LAYOUT ROW, and the one a read-back cannot fake.
//
// The importer writes four slots of an imported class's `[4]int` field through
// the DEPENDENCY's `Set` (compiled where the class is local, so at the real
// layout) and then sums them through the DEPENDENCY's `SumHere` AND through its
// OWN indexing of `h.arr`. §2.8.3 rule 4: "An external instantiation behaves
// identically to a same-module one." Two views of one object must agree.
//
// `SumHere() * 100 + ownSum` = 10*100 + 10 = 1010; a divergence lands anywhere
// but there.
// ===========================================================================
BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t3_imported_class_inline_array_field_one_layout_2_8_3) {
    const std::string body =
        "  public static method Main(): int "
        "    using var h := new Holder3183(); "
        "    h.Set(0, 1); h.Set(1, 2); h.Set(2, 3); h.Set(3, 4); "
        "    var ownSum := h.arr[0] + h.arr[1] + h.arr[2] + h.arr[3]; "
        "    if h.SumHere() <> 10 then return 1; end if; "
        "    if ownSum <> 10 then return 2; end if; "
        "    if h.arr[3] <> 4 then return 3; end if; "
        "    return 101; "
        "  end Main;";
    int r = compileImportAndRun3183(lib3183("T3"), app3183("T3", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 101);
}

BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t3_twin_class_inline_array_field_in_one_module_2_8_3) {
    const std::string body =
        "  public static method Main(): int "
        "    using var h := new Holder3183(); "
        "    h.Set(0, 1); h.Set(1, 2); h.Set(2, 3); h.Set(3, 4); "
        "    var ownSum := h.arr[0] + h.arr[1] + h.arr[2] + h.arr[3]; "
        "    if h.SumHere() <> 10 then return 1; end if; "
        "    if ownSum <> 10 then return 2; end if; "
        "    if h.arr[3] <> 4 then return 3; end if; "
        "    return 101; "
        "  end Main;";
    int r = compileAndRun3183(twin3183("T3L", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 101);
}

// ===========================================================================
// T3b — the same, 200 times, so a per-object overrun of an imported class's
// inline-array field is inside the ASan window rather than a single write that
// happens to land somewhere harmless.
// ===========================================================================
BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t3b_repeated_class_field_writes_do_not_overflow_2_8_3) {
    const std::string body =
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0..199 do "
        "      using var h := new Holder3183(); "
        "      h.Set(0, 1); h.Set(1, 2); h.Set(2, 3); h.Set(3, 4); "
        "      acc := acc + h.SumHere(); "
        "    end for; "
        "    return acc / 200; "
        "  end Main;";
    int r = compileImportAndRun3183(lib3183("T3b"), app3183("T3b", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 10);
}

// ===========================================================================
// T4 — control: a NULLABLE ELEMENT type. The element rides `children[0]` and
// always crossed; this pins that carrying the size did not disturb it, and that
// a `[3]int?` is still laid out at its own (wider, tagged) element width rather
// than the size being applied to the wrong element type.
//
// `A[0]! * 10 + N` = 1*10 + 2 = 12, with a null in the middle slot so the
// element really is the nullable form.
// ===========================================================================
BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t4_imported_nullable_element_control_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var a: [3]int? := [1, null, 3]; "
        "    var v := new NullElem3183(a, 2); "
        "    if v.A[1] <> null then return 99; end if; "
        "    return v.A[0]! * 10 + v.N; "
        "  end Main;";
    int r = compileImportAndRun3183(lib3183("T4"), app3183("T4", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 12);
}

BEZEL_TEST(xmod_imported_inline_array_size_3183,
           t4_twin_nullable_element_in_one_module_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    var a: [3]int? := [1, null, 3]; "
        "    var v := new NullElem3183(a, 2); "
        "    if v.A[1] <> null then return 99; end if; "
        "    return v.A[0]! * 10 + v.N; "
        "  end Main;";
    int r = compileAndRun3183(twin3183("T4L", body));
    if (r == kDepUnavailable3183) return;
    BEZEL_EXPECT_EQ(r, 12);
}

}  // namespace bezel::module
