// Codegen — fix #4036 (§12.5, v0.295): `Emitter::ensurePropertyAccessorFn`
// synthesized a LIVE, null-returning stub function for an abstract-shorthand
// NAMED property (`abstract property Name: T;`) and installed it in the
// owning (abstract) class's own vtable/itable slot, instead of leaving the
// slot null the way an abstract METHOD's own slot already is.
//
// THE DEFECT. `pd->bodyKind == PropertyBodyKind::Auto` for BOTH a real
// auto-property and the abstract shorthand (Parser_Decls.cpp — neither form
// has `get`/`set`/`=>` after the type, so both fall into the same parse
// arm). `ensurePropertyAccessorFn`'s getter/setter `isAuto` branches did not
// distinguish them: for the abstract shorthand, #4020's
// `isAutoPropertyStorageSymbol` already excludes it from `storage` (no
// backing field), so `fieldIndex_.find(ps)` misses and the fallback
// `builder_.CreateRet(llvm::Constant::getNullValue(valTy))` builds a REAL
// function — installed as the vtable slot's value — instead of the
// `return nullptr` an abstract METHOD's absent body already produces
// (methods: no `MethodBodyKind::Abstract` body to emit at all; indexers:
// `ensureIndexerGetterFn`/`ensureIndexerSetterFn` have no "Auto" branch to
// fall into in the first place — structurally unreachable there, per the
// issue's own investigation).
//
// NOT independently observed as reachable (see the issue and the commit
// this test accompanies): #4020 requires every concrete class to override
// an inherited abstract property, so the abstract class's OWN vtable is
// never installed as any live object's runtime vtable — an abstract class
// cannot be `new`'d. This fix is therefore defense-in-depth (mirroring the
// #3641/row-12 precedent for methods/indexers — leave the slot null so a
// FUTURE sema gap fails loudly with a null-pointer fault, not a silent
// wrong value), not a fix for an observed wrong-value or crash bug. The
// tests below are STRUCTURAL (inspecting the emitted IR directly, the same
// technique `case_stmt_total_dead_edge_trap_3073_test.cpp` and
// `static_init_atomic_1391_test.cpp` use for codegen-only changes with no
// Bezel-level syntax to assert on) plus RUNTIME ACCEPT-SIDE controls proving
// the fix does not disturb a normal, correctly-overridden abstract property.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// Same resolution technique as case_stmt_total_dead_edge_trap_3073_test.cpp:
// `bezelc` sits alongside this test binary's build tree.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compiles `source` to an object (no link, no Main call required) with
// BEZEL_DUMP_IR=1 set, capturing the LLVM module text via a `bezelc`
// SUBPROCESS (popen) — see the 3073 test's identical helper for why a
// subprocess is used instead of an in-process Driver call (LLVM's `errs()`
// raw_fd_ostream capture is unreliable in-process).
std::string compileAndCaptureIR(const std::string& source) {
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_4036");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string obj = (dir / (stem + ".o")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    std::string cmd = "BEZEL_DUMP_IR=1 '" + bezelc + "' -c -o '" + obj +
                      "' '" + src + "' 2>&1";
    std::string text;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        std::array<char, 8192> chunk{};
        std::size_t got;
        while ((got = fread(chunk.data(), 1, chunk.size(), p)) > 0) {
            text.append(chunk.data(), got);
        }
        pclose(p);
    }
    std::remove(src.c_str());
    std::remove(obj.c_str());
    return text;
}

// Finds the single-line `@"vt$<Module>.<Class>" = constant [N x ptr] [...]`
// global definition and returns just the bracketed element list (the `[...]`
// text), or "" if not found. The vtable is emitted as one `constant` literal
// per class (LlvmBackend_Setup.cpp declareVtables), so this is a simple,
// robust anchor — no need to disambiguate call sites vs definitions the way
// sliceFunctionBody (3073 test) does for a callable body.
std::string sliceVtableElements(const std::string& ir,
                                const std::string& moduleName,
                                const std::string& className) {
    std::string marker =
        "@\"vt$" + moduleName + "." + className + "\" = constant";
    std::size_t start = ir.find(marker);
    if (start == std::string::npos) return "";
    std::size_t open = ir.find('[', ir.find('[', start) + 1);  // second '['
    if (open == std::string::npos) return "";
    // Find the matching closing ']' for the element-list '[' by scanning to
    // end of line (the whole vtable is emitted on one line).
    std::size_t eol = ir.find('\n', open);
    if (eol == std::string::npos) return "";
    std::size_t lastBracket = ir.rfind(']', eol);
    if (lastBracket == std::string::npos || lastBracket < open) return "";
    return ir.substr(open, lastBracket - open + 1);
}

} // namespace

// ---------------------------------------------------------------------------
// STRUCTURAL — the abstract-shorthand NAMED property's getter slot is
// `ptr null`, exactly like the sibling abstract METHOD's slot, and no
// `$Current$propget` function is defined anywhere in the module (pre-fix it
// synthesized one).
// ---------------------------------------------------------------------------

BEZEL_TEST(abstract_property_shorthand_null_vtable_slot_4036,
           abstract_named_property_vtable_slot_is_null_spec_12_5) {
    const std::string src =
        "module M; "
        "class Widget4036 end Widget4036; "
        "public abstract class BaseInit4036 "
        "  public abstract property Current: Widget4036; "
        "end BaseInit4036; "
        "class C4036 "
        "  public static method Main(): int return 0; end Main; "
        "end C4036;";
    std::string ir = compileAndCaptureIR(src);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string elems = sliceVtableElements(ir, "M", "BaseInit4036");
    BEZEL_EXPECT_TRUE(!elems.empty());
    // The getter slot is the LAST element of the vtable array (declareVtables
    // appends property/indexer slots after the four Object-virtual slots).
    BEZEL_EXPECT_TRUE(elems.find("ptr null]") != std::string::npos);
    // No live stub function was synthesized for the getter at all.
    BEZEL_EXPECT_TRUE(ir.find("$Current$propget") == std::string::npos);
}

// CONTROL — the sibling abstract METHOD's own slot, same class shape,
// proves the harness parses the vtable literal correctly (both should be
// null; this pins the baseline the property fix now matches).
BEZEL_TEST(abstract_property_shorthand_null_vtable_slot_4036,
           abstract_method_vtable_slot_is_null_control_spec_12_5) {
    const std::string src =
        "module M; "
        "public abstract class BaseM4036 "
        "  public abstract method Area(): int; "
        "end BaseM4036; "
        "class C4036 "
        "  public static method Main(): int return 0; end Main; "
        "end C4036;";
    std::string ir = compileAndCaptureIR(src);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string elems = sliceVtableElements(ir, "M", "BaseM4036");
    BEZEL_EXPECT_TRUE(!elems.empty());
    BEZEL_EXPECT_TRUE(elems.find("ptr null]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// RUNTIME ACCEPT-SIDE CONTROLS — the fix must not disturb a normal,
// correctly-overridden abstract property (the ordinary, already-working
// shape #4018/#4020 cover). Both an expression-bodied override and a
// concrete class satisfying the obligation two levels up still dispatch
// correctly through the OVERRIDE's own vtable slot.
// ---------------------------------------------------------------------------

BEZEL_TEST(abstract_property_shorthand_null_vtable_slot_4036,
           overridden_abstract_property_still_dispatches_correctly_spec_12_5) {
    auto r = bezel::test::runAot(
        "module M; "
        "public abstract class BaseInit4036B "
        "  public abstract property Current: int; "
        "end BaseInit4036B; "
        "class Derived4036B : BaseInit4036B "
        "  public override property Current: int => 42; "
        "end Derived4036B; "
        "class C4036B "
        "  public static method Main(): int "
        "    var b: BaseInit4036B := new Derived4036B(); "
        "    return b.Current; "
        "  end Main; "
        "end C4036B;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// A THREE-level chain: the abstract base's own slot is null throughout, and
// the leaf's override is what every live object's vtable actually installs
// — no intermediate abstract class's slot is ever read.
BEZEL_TEST(abstract_property_shorthand_null_vtable_slot_4036,
           abstract_property_satisfied_two_levels_up_still_correct_spec_12_5) {
    auto r = bezel::test::runAot(
        "module M; "
        "public abstract class Root4036 "
        "  public abstract property Name: int; "
        "end Root4036; "
        "public abstract class Mid4036 : Root4036 "
        "end Mid4036; "
        "class Leaf4036 : Mid4036 "
        "  public override property Name: int => 7; "
        "end Leaf4036; "
        "class C4036C "
        "  public static method Main(): int "
        "    var r: Root4036 := new Leaf4036(); "
        "    return r.Name; "
        "  end Main; "
        "end C4036C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
