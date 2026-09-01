// Cross-module regression tests for fix #3581 — an imported §11.6 NESTED type
// named in a DEPENDENCY's OWN public signature must resolve across the
// artifact boundary (not just within a single artifact, which is what #3540
// already closed).
//
// AUTHORITY.
//
// spec §11.6 (v0.286), "Nested types", verbatim:
//
//     "A nested type is referenced from outside as `Outer.Nested`."
//
// spec §2.8.3 rule 3 (v0.286), "Program-wide type identity", verbatim:
//
//     "A type is identified program-wide by its **declaring module name, its
//      qualified type name (including any enclosing-type path, [§11.6]), and
//      its generic arity**"
//
// spec §2.8.3 rule 1 (v0.286): the build supplies the transitive closure, and
// an interface's references include those made by the declaration bodies it
// carries — this shape is exactly what the closure rule contemplates.
//
// THE DEFECT, MEASURED on d1b480f (HEAD before this fix) with a real
// three-artifact build. `TBase` declares `Outer.Box`. `TMidC` imports `TBase`
// and names `Outer.Box` in its OWN public signature
// (`Mid.Make(c: int): owned Outer.Box`). A consumer of `TMidC` then compiles
// against that signature. Pre-fix:
//
//   * `SemaContext::registerImportedType` was called only for top-level
//     types (`ModuleLoader.cpp` pass 1), so `importedByModule_["TBase"]`
//     held `Outer` and nothing else — `Outer.Box` was never pushed at all.
//   * `SemaContext::lookupImportedTypeByName` matched on the SIMPLE name
//     `t->name == name`, so even if `Outer.Box` had been pushed the lookup
//     for `"Outer.Box"` would have walked a vector whose only entry's name
//     was `"Outer"` and returned null.
//   * `ModuleLoader::Loader::lookupNamed` then fell through to that null,
//     the TypeRef's `declaration` stayed null, and codegen took the i32
//     default (#495's silent truncation):
//
//       # C1 / C2 — cross-artifact, DEFECTIVE (32-bit argument):
//         36c:  8b 7c 24 14           mov    0x14(%rsp),%edi
//
//     The callee's definition takes a pointer. The program "works" whenever
//     the heap address fits in 32 bits — which it does for the repro — so a
//     stdout-comparison oracle calls it PASS. It is not.
//
// The shape has TWO FACES — quiet (C1 / C2, prints 55 because the heap
// happens to fit) and loud (C3, IR verification failure because the
// consumer ALSO names the type, so `new Outer.Box(55)` lands in a context
// where the i32 collapse is observable). The quiet one is the dangerous
// one — the program runs and answers the wrong width silently.
//
// THREE CONSUMER ROWS, ONE PER SHAPE:
//
//   * C1 — consumer does NOT import TBase and does NOT name Outer.Box. The
//     build "succeeds" pre-fix and truncates silently. Post-fix the
//     consumer's loader STILL cannot find Outer.Box because it has no
//     TBase metadata (transitive closure requires it). C1 therefore
//     EXPECTS the same quiet shape it had pre-fix — the issue itself notes
//     this row's "transitive closure" character and treats the resolution
//     gap as a soundness widening only when the consumer actually has
//     TBase's metadata to resolve against. The row pins the consumer-side
//     requirement (must import TBase to get 64-bit pointer types).
//
//   * C2 — consumer imports TBase but does NOT name Outer.Box. Pre-fix the
//     parameter is silently truncated to i32. Post-fix the parameter is a
//     pointer, `Mid.Take(b)` is called with the full 64-bit address, the
//     heap object survives the call and `b.Cents()` returns the right
//     number. This is THE row the fix changes.
//
//   * C3 — consumer imports TBase AND names Outer.Box in its source
//     (`new Outer.Box(55)`). Pre-fix the build fails IR verification. Post-
//     fix the build succeeds and runs.
//
// The C2 row's stdout is the same pre-fix and post-fix (the program prints
// 55 either way because the heap address happens to be < 4 GB). To pin the
// argument WIDTH rather than the value, every C2-style row additionally
// asserts the disassembly contains a 64-bit `mov …, %rdi` and does NOT
// contain the pre-fix `mov 0x14(%rsp), %edi`. An argument-WIDTH oracle is
// the only one that distinguishes the pre-fix and post-fix shapes for this
// defect — the issue cites the disassembly explicitly.
//
// WHY THE KEY MUST BE QUALIFIED. §2.8.3 rule 3 identifies a type by its
// qualified name; #3529 is the record of `Outer1.Box` and `Outer2.Box`
// being two unrelated types under that rule. Keying on the simple name
// would make one answer for the other — the same collision #3529 removed
// from the linker, reintroduced in the importer's cross-module type table.
// `TypeSymbol::qualifiedTypeName()` is the one authority for the spelling,
// exactly mirroring how #3540 keyed `localTypes`.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process scratch dir under the shared base, cleaned up at exit — never
// fs::temp_directory_path(), which is the tmpfs CLAUDE.md forbids for scratch.
const fs::path& wtDir3581() { return wtScratchDir("wt-3581"); }

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Three-artifact build result: two libraries (TBase, TMidC) plus a consumer
// that compiles against both, links, runs, and (separately) returns the
// consumer's object disassembly so the test can assert argument width.
//
// `consumerImportsTBase` and `consumerNamesOuterBox` are NOT flags for the
// build driver — every consumer is compiled and linked against BOTH library
// artifacts (per spec §2.8.3 rule 1, transitive closure). The flags are
// kept as test-name suffixes so a reader sees which CONSUMER-SOURCE
// variation a row covers: the issue's three rows differ only in whether
// the consumer's own source imports TBase or names Outer.Box. The build
// driver always supplies both — otherwise C1 would not even LINK
// (`TMidC.Mid.Take` calls `Outer.Box$Cents`, which is defined in TBase.o
// and the consumer's link would have no symbol for it).
struct ThreeRunOut {
    int status = -1;
    std::string out;
    std::string objdump;  // `objdump -d -r` of the consumer object
};

// runObjdump — execute `objdump -d -r` against `objectPath`, capture stdout.
// `objdump` is a system tool; the path is taken from PATH (the bezelc build
// does not link its own.
std::string runObjdump(const std::string& objectPath) {
    std::string cmd = "objdump -d -r " + objectPath + " 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {};
    std::array<char, 4096> buf{};
    std::string out;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p))
        out.append(buf.data());
    int rc = ::pclose(p);
    if (rc != 0) return {};
    return out;
}

// lineBeforeMidTakeCall — find the `mov` instruction immediately preceding
// the `call bz$TMidC.Mid$Take` site in the consumer object. Returns the
// instruction text (empty when no such site exists).
//
// GNU `objdump -d -r` emits each call site as TWO lines:
//
//   <addr>:^I<bytes>^Icall  <target>                       ← instruction
//   ^I^I^I<addr>: R_X86_64_PLT32^I<symbol>-0x4              ← relocation
//
// (where `^I` is a literal tab and the relocation line has a leading
// indentation of three tabs). The relocation symbol `bz$TMidC.Mid$Take`
// is in the SECOND line; the first line's `call` mnemonic has the
// in-section target (here `bz$AppC2.Program$Main`), not the cross-module
// callee. So a `find(bz$TMidC.Mid$Take) && find(call)` test on a single
// line is unsatisfiable — the two halves of the call site live on
// adjacent lines.
//
// The walker below collects EVERY disassembled-instruction line into a
// ring of the last two, and returns the line BEFORE the most recent
// instruction line at the moment the relocation comment for
// `bz$TMidC.Mid$Take` arrives. The two-line shape is:
//
//   ring[0] = argument-load  (e.g. `mov (%rsp),%rdi`)
//   ring[1] = `call ...`     (instruction we are about to relocate)
//   next    = `^I^I^I<addr>: R_X86_64_PLT32 bz$TMidC.Mid$Take-0x4`
//
// The argument-load is the one BEFORE the `call`, which is two slots
// back from the relocation.
//
// The defect's signature is "32-bit `mov ..., %edi`" — a 64-bit pointer
// truncated to the low 32 bits and passed in the EDI register (System V
// AMD64 calling convention for integer/pointer args). Post-fix the
// instruction is a 64-bit `mov ..., %rdi` load.
std::string lineBeforeMidTakeCall(const std::string& objdump) {
    std::istringstream in(objdump);
    std::string prev1, prev2;  // ring of the last two instruction lines
    std::string line;
    while (std::getline(in, line)) {
        // Relocation-comment lines look like:
        //   "^\t^\t^\t<addr>: R_X86_64_<kind>^\t<symbol>-<delta>"
        // They begin with whitespace, contain `:` and a tab, and have
        // `R_X86_64_` in their opcode column.
        const bool looksLikeInst =
            !line.empty() && (line[0] == ' ' || line[0] == '\t') &&
            line.find(':') != std::string::npos &&
            line.find('\t') != std::string::npos;
        const bool isRelocationForMidTake =
            looksLikeInst &&
            line.find("R_X86_64_") != std::string::npos &&
            line.find("bz$TMidC.Mid$Take") != std::string::npos;
        if (isRelocationForMidTake) {
            // ring[1] is the `call`, ring[0] is the argument-load. We want
            // the instruction TWO slots back — the one BEFORE the call.
            return prev2;
        }
        if (looksLikeInst) {
            // Skip relocation-only lines for the ring so we do not
            // confuse `R_X86_64_PLT32` for a real instruction.
            const auto tabPos = line.rfind('\t');
            const std::string opcodeCol =
                tabPos == std::string::npos ? std::string{}
                                            : line.substr(tabPos + 1);
            if (opcodeCol.compare(0, 8, "R_X86_64_") != 0) {
                prev2 = prev1;
                prev1 = line;
            }
        }
    }
    return {};
}

// hasRegOperand — does `inst` end in a register-operand move, e.g. `%rdi`
// for a 64-bit load or `%edi` for a 32-bit load. The disassembly text
// format is `<addr>: <bytes> <mnemonic> <operands>`, with the operands
// separated by tabs and commas. We inspect the operands column.
bool movEndsWithReg(const std::string& inst, const std::string& reg) {
    if (inst.empty()) return false;
    // Strip the leading address/bytes column — find the last tab.
    const auto lastTab = inst.rfind('\t');
    if (lastTab == std::string::npos) return false;
    const std::string ops = inst.substr(lastTab + 1);
    if (ops.find("mov") != 0) return false;
    // Operand should end with the register.
    const auto commaPos = ops.rfind(',');
    if (commaPos == std::string::npos) return false;
    std::string lastOp = ops.substr(commaPos + 1);
    // Trim leading whitespace.
    while (!lastOp.empty() && lastOp.front() == ' ') lastOp.erase(0, 1);
    return lastOp == reg;
}

// buildThree — compile `tbaseSrc` and `tmidcSrc` to objects, compile the
// consumer `consumerSrc` against BOTH, link, run, AND dump the consumer
// object's disassembly. The disassembly is what proves the silent truncation
// has gone — stdout alone cannot, because the program prints 55 either way
// when the heap address fits in 32 bits.
ThreeRunOut buildThree(const std::string& tbaseSrc,
                       const std::string& tmidcSrc,
                       const std::string& consumerSrc,
                       const std::string& tag,
                       bool consumerImportsTBase,
                       bool consumerNamesOuterBox) {
    ThreeRunOut r;
    auto dir = wtDir3581();
    std::string stem = "bz3581_" + tag + "_" + unique_stem("");

    bezel::Driver driver;

    TempFile baseSrc((dir / (stem + "_base.bzl")).string());
    TempFile baseObj((dir / (stem + "_base.o")).string());
    writeSource(baseSrc.path, tbaseSrc);
    if (!driver.compileObject(baseSrc.path, baseObj.path).success) return r;

    // TMidC must compile against TBase so its `Outer.Box` reference in
    // `Mid.Make(c: int): owned Outer.Box` resolves to the right symbol
    // and the metadata it carries carries the qualified name. Without
    // this dep, TMidC's own metadata would not bind `Outer.Box` and the
    // consumer-side defect (the issue's #3581 case) would never be
    // reachable — the chain would be broken at the producer step.
    std::vector<bezel::Dependency> midDeps{{baseObj.path, baseObj.path}};
    TempFile midSrc((dir / (stem + "_mid.bzl")).string());
    TempFile midObj((dir / (stem + "_mid.o")).string());
    writeSource(midSrc.path, tmidcSrc);
    if (!driver.compileObject(midSrc.path, midObj.path, midDeps).success)
        return r;

    // The dependency vector MUST carry both libraries for the cross-module
    // lookup to find TBase.Outer.Box at all (spec §2.8.3 rule 1, transitive
    // closure) AND for the link step to find `Outer.Box$Cents`'s definition.
    // The issue's three rows differ in CONSUMER-SOURCE contents (whether the
    // source `import`s TBase or names Outer.Box), not in build flags — every
    // consumer is compiled and linked against both libraries.
    std::vector<bezel::Dependency> deps{
        {midObj.path, midObj.path},
        {baseObj.path, baseObj.path},
    };
    (void)consumerImportsTBase;
    (void)consumerNamesOuterBox;

    TempFile consSrc((dir / (stem + "_cons.bzl")).string());
    TempFile consExe((dir / stem).string());
    TempFile consOut((dir / (stem + ".out")).string());
    TempFile consObj((dir / (stem + "_cons.o")).string());
    writeSource(consSrc.path, consumerSrc);
    if (getenv("BEZEL_AOT_KEEP")) {
        std::ofstream dbgSrc((dir / (stem + "_cons.SRC")).string());
        dbgSrc << consumerSrc;
    }

    // Emit the consumer object FIRST (compileObject keeps the .o, no link).
    // The disassembly comes from this object so the call-site argument width
    // is observable — a linked executable's relocation has been resolved
    // already and the call site's %rdi/%edi split is what proves whether the
    // parameter type was a pointer or an i32 in the original emitted code.
    if (!driver.compileObject(consSrc.path, consObj.path, deps).success)
        return r;
    r.objdump = runObjdump(consObj.path);
    // Diagnostic dump so a failure points to the actual bytes.
    if (getenv("BEZEL_AOT_KEEP")) {
        std::ofstream dbg((dir / (stem + "_cons.dump")).string());
        dbg << r.objdump;
    }

    // Then compile+link the executable for status+stdout. The two paths
    // share the same front-end / mid-end, so a regression in either shows
    // up here.
    if (!bezel::test::compileFileForTest(driver, consSrc.path, consExe.path,
                                          deps)
             .success)
        return r;

    std::string cmd = consExe.path + " > " + consOut.path + " 2>&1";
    int status = std::system(cmd.c_str());
    {
        std::ifstream in(consOut.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        r.out = ss.str();
    }
    if (status == -1) return r;
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
    return r;
}

// The three library fragments, identical across C1/C2/C3, so any difference
// across the three rows is purely consumer-side.
//
// TBase: declares `Outer` and a §11.6 nested `Outer.Box` with a single int
// payload and a `Cents()` accessor. Identical to #3540's `LibS.Outer.Box`
// shape so the cross-module path is the same.
constexpr const char* kTBase =
    "module TBase; "
    "public class Outer "
    "  public class Box "
    "    private _c: int; "
    "    public constructor(c: int) _c := c; end constructor; "
    "    public method Cents(): int => _c; "
    "  end Box; "
    "end Outer; ";

// TMidC: imports TBase, declares a `Mid` with two methods naming
// `Outer.Box` in their signatures. THIS is the cross-artifact gap #3540
// did NOT close: TMidC is a separate artifact from TBase.
constexpr const char* kTMidC =
    "module TMidC; "
    "import TBase; "
    "public class Mid "
    "  public static method Make(c: int): owned Outer.Box => new Outer.Box(c); "
    "  public static method Take(b: Outer.Box): int => b.Cents(); "
    "end Mid; ";

// Consumer that prints `C<n>=<value>` and returns 0. The three flavors vary
// only in the consumer's own source; the libraries are constant.
std::string consumerC1() {
    return
        "module AppC1; "
        "import TMidC; "
        "class Program public static method Main(): int "
        "  using var b := Mid.Make(55); "
        "  Console.WriteLine($\"C1={Mid.Take(b)}\"); "
        "  return 0; "
        "end Main; end Program; ";
}
std::string consumerC2() {
    return
        "module AppC2; "
        "import TMidC; "
        "import TBase; "
        "class Program public static method Main(): int "
        "  using var b := Mid.Make(55); "
        "  Console.WriteLine($\"C2={Mid.Take(b)}\"); "
        "  return 0; "
        "end Main; end Program; ";
}
std::string consumerC3() {
    return
        "module AppC3; "
        "import TMidC; "
        "import TBase; "
        "class Program public static method Main(): int "
        "  using var b := Mid.Make(55); "
        "  using var d := new Outer.Box(2); "
        "  Console.WriteLine($\"C3={Mid.Take(b)} d={d.Cents()}\"); "
        "  return 0; "
        "end Main; end Program; ";
}

}  // namespace

// -- The defect ------------------------------------------------------------

// spec §11.6 / §2.8.3 rule 3 / §2.8.2, v0.286, #3581 — consumer does NOT
// import TBase in its own SOURCE, but the BUILD supplies the transitive
// closure (`--import tbase.o --import tmid.o`) so the consumer's loader
// has TBase's metadata and CAN resolve `Outer.Box`. Pre-fix the program
// ran and printed `C1=55` but the disassembly showed a 32-bit `mov …,
// %edi` argument load — the silent-truncation face of the defect (the
// heap address happened to fit in 32 bits so the value was right). Post-
// fix the resolution actually completes, the parameter is a 64-bit
// pointer, and the argument load is `mov …, %rdi`. This is the issue's
// own measurement: "C1 prints C1=55 — but see the disassembly: ptr
// truncated to i32".
BEZEL_TEST(xmod_cross_artifact_nested_type_in_signature_3581,
           consumer_without_tbase_in_source_resolves_via_closure) {
    ThreeRunOut r = buildThree(kTBase, kTMidC, consumerC1(), "c1",
                               /*consumerImportsTBase=*/false,
                               /*consumerNamesOuterBox=*/false);
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("C1=55\n"));
    // Post-fix: 64-bit argument load (mov …, %rdi). Pre-fix: 32-bit (mov
    // …, %edi) — the silent truncation the disassembly exposes.
    const std::string c1Load = lineBeforeMidTakeCall(r.objdump);
    BEZEL_EXPECT_FALSE(c1Load.empty());
    if (getenv("BEZEL_AOT_KEEP"))
        fprintf(stderr, "c1Load='%s'\n", c1Load.c_str());
    BEZEL_EXPECT_TRUE(movEndsWithReg(c1Load, "%rdi"));
    BEZEL_EXPECT_FALSE(movEndsWithReg(c1Load, "%edi"));
}

// spec §11.6 / §2.8.3 rule 3 / §2.8.2, v0.286, #3581 — THE ROW the fix
// changes. Consumer imports TBase transitively, so the consumer's loader
// has TBase's metadata and `lookupImportedTypeByName("Outer.Box")` can
// resolve via the upgraded shell path. Pre-fix the parameter is silently
// truncated to i32; post-fix the parameter is a 64-bit pointer. The
// program's stdout is the same in both cases (the heap address fits in
// 32 bits), so the disassembly is the only oracle — exactly the argument
// in the issue's table for C2.
BEZEL_TEST(xmod_cross_artifact_nested_type_in_signature_3581,
           consumer_with_tbase_in_closure_resolves_64bit_pointer) {
    ThreeRunOut r = buildThree(kTBase, kTMidC, consumerC2(), "c2",
                               /*consumerImportsTBase=*/true,
                               /*consumerNamesOuterBox=*/false);
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("C2=55\n"));
    // Post-fix: a 64-bit load of the pointer argument (mov …, %rdi).
    // Pre-fix: a 32-bit load (mov …, %edi). The disassembly is the only
    // oracle that distinguishes the two — both prints "C2=55" because the
    // heap address fits in 32 bits.
    const std::string c2Load = lineBeforeMidTakeCall(r.objdump);
    BEZEL_EXPECT_FALSE(c2Load.empty());
    if (getenv("BEZEL_AOT_KEEP"))
        fprintf(stderr, "c2Load='%s'\n", c2Load.c_str());
    BEZEL_EXPECT_TRUE(movEndsWithReg(c2Load, "%rdi"));
    BEZEL_EXPECT_FALSE(movEndsWithReg(c2Load, "%edi"));
}

// spec §11.6 / §2.8.3 rule 3 / §2.8.2, v0.286, #3581 — consumer imports
// TBase transitively AND names `Outer.Box` in its own source
// (`new Outer.Box(2)`). Pre-fix the build fails IR verification with
// "Call parameter type does not match function signature!" (the loud face
// of the defect). Post-fix the build succeeds and runs.
BEZEL_TEST(xmod_cross_artifact_nested_type_in_signature_3581,
           consumer_that_names_outer_box_in_its_own_source_builds) {
    ThreeRunOut r = buildThree(kTBase, kTMidC, consumerC3(), "c3",
                               /*consumerImportsTBase=*/true,
                               /*consumerNamesOuterBox=*/true);
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("C3=55 d=2\n"));
    // Post-fix: argument to Mid.Take is a 64-bit pointer (the consumer also
    // names Outer.Box in its own source, so the IR verifier would have
    // caught a 32-bit call against a pointer-parameter declaration; the
    // disassembly check makes the resolution explicit either way).
    const std::string c3Load = lineBeforeMidTakeCall(r.objdump);
    BEZEL_EXPECT_FALSE(c3Load.empty());
    BEZEL_EXPECT_TRUE(movEndsWithReg(c3Load, "%rdi"));
    BEZEL_EXPECT_FALSE(movEndsWithReg(c3Load, "%edi"));
}

// -- Control: same-artifact case (already fixed by #3540) ------------------

// CONTROL (spec §11.6 / §2.8.3 rule 3, v0.286) — the SAME-ARTIFACT shape
// that #3540 closed. A method on a nested class whose parameter is that
// same nested type, all in one module. This row pins "same-artifact stays
// working" — a fix that changed this would have widened past its subject.
BEZEL_TEST(xmod_cross_artifact_nested_type_in_signature_3581,
           same_artifact_nested_type_in_signature_control) {
    // A second, single-artifact build path: no transitive closure, only the
    // library compiles and the consumer is folded in. We piggy-back on
    // buildThree with TMidC playing the role of "the library" and the
    // consumer not importing TBase — the same shape as #3540's self row.
    ThreeRunOut r = buildThree(
        "module LibS; "
        "public class Outer "
        "  public class Box "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method Plus(other: Outer.Box): int => _v + other.Value(); "
        "  end Box; "
        "end Outer; ",
        // kTMidC is unused here; we replace it with an empty stub library.
        "module Stub; public class Empty end Empty; ",
        "module AppSelf; import LibS; "
        "class Program public static method Main(): int "
        "  using var a := new Outer.Box(7); "
        "  using var b := new Outer.Box(5); "
        "  Console.WriteLine($\"self={a.Plus(b)}\"); "
        "  return 0; "
        "end Main; end Program; ",
        "selfctrl",
        /*consumerImportsTBase=*/false,
        /*consumerNamesOuterBox=*/false);
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("self=12\n"));
}

// CONTROL (spec §2.8.3 rule 3, v0.286) — a top-level type's self-typed
// parameter is unchanged by this fix (a top-level type's qualified name
// equals its simple name, so the lookup match was already correct).
// Measured working pre-fix; must keep passing or the fix has widened.
BEZEL_TEST(xmod_cross_artifact_nested_type_in_signature_3581,
           top_level_type_cross_module_control) {
    ThreeRunOut r = buildThree(
        "module TBaseTop; "
        "public class Box "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "end Box; ",
        "module TMidTop; "
        "import TBaseTop; "
        "public class Mid "
        "  public static method Make(v: int): owned Box => new Box(v); "
        "  public static method Take(b: Box): int => b.Value(); "
        "end Mid; ",
        "module AppTop; import TMidTop; import TBaseTop; "
        "class Program public static method Main(): int "
        "  using var b := Mid.Make(11); "
        "  Console.WriteLine($\"top={Mid.Take(b)}\"); "
        "  return 0; "
        "end Main; end Program; ",
        "topctrl",
        /*consumerImportsTBase=*/true,
        /*consumerNamesOuterBox=*/false);
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("top=11\n"));
}

}  // namespace bezel::module
