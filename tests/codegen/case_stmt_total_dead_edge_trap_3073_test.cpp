// fix #3073 — §21.7 statement-`case` totality grant, the DEAD-EDGE LOWERING.
//
// Spec §21.7 last sentence: "For a `case` granted totality under this rule,
// the no-match edge is statically dead; **an implementation lowers it as a
// defined trap (raising `MatchFailureException` or aborting) rather than
// fall-through**, so that the grant's soundness never rests on the
// exhaustiveness computation alone (as-if: the trap is unreachable in a
// conforming implementation)."
//
// What was wrong: sema's `ExprTC::stmtAlwaysExits` (ExprTypecheck.cpp, the
// `ast::CaseStmt` arm) COMPUTED the grant locally and consumed it — never
// recorded it on the AST or a side table — so codegen's statement-`case`
// lowering could not distinguish a granted `case` from any other. The
// no-match edge branched to `casestmt.end`, control reached the end of the
// callable, and `Emitter::emitImplicitReturn` emitted `ret <zero>` for a
// value-returning function. The edge was *defined* (a zero return) but not
// a *trap*: a bug in the exhaustiveness computation degraded silently to a
// wrong value, defeating §21.7's guarantee.
//
// The fix persists the grant as `ast::CaseStmt::totalityGranted` (set by
// `stmtAlwaysExits` on the verdict paths) and has
// `Emitter::lowerCaseStmt` (LlvmBackend_Stmts.cpp) consult it: when true,
// the no-match edge emits `MatchFailureException` instead of falling
// through to `casestmt.end`.
//
// These four tests pin the lowering for each §21.7 closed-value-space
// grant (enum post-#3044, choice post-#2129, bool + sealed hierarchy
// post-#2133). They inspect the LLVM IR via `BEZEL_DUMP_IR` (the same
// mechanism `static_init_atomic_1391_test.cpp` uses for #1391's atomic
// CAS — codegen-only changes need IR inspection; there is no Bezel-level
// syntax to assert on). Reverting the codegen half (the `lowerCaseStmt`
// change) makes every assertion fail; reverting the sema half (the AST
// write in `stmtAlwaysExits`) also fails them, since `totalityGranted`
// stays false and the codegen guard never fires. Both halves are
// required to make this test green — the issue's spec-cited "the grant's
// soundness never rests on the exhaustiveness computation alone" is a
// TWO-PART guarantee.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// The `bezelc` binary sits alongside this test binary's build tree:
// `<build>/tests/bezel_tests_codegen` (this process) and `<build>/bezelc`
// (its sibling under the SAME build dir, one level up). Resolved via
// /proc/self/exe rather than a hardcoded path so this works from any
// worktree's own build directory.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compiles `source` to an object (no link, no Main required) with
// BEZEL_DUMP_IR=1 set, capturing the LLVM module text the compiler prints
// to stderr (LlvmBackend.cpp: `if (... getenv("BEZEL_DUMP_IR"))
// m->print(errs()...)`). Runs the real `bezelc` binary as a SUBPROCESS
// (via popen) rather than calling Driver in-process: LLVM's `errs()` is a
// raw_fd_ostream and capturing it by redirecting this test process's own
// fd 2 around the call was unreliable (buffering/timing not fully in
// this test's control, confirmed in the #1391 hand-off). A subprocess's
// stderr is captured to completion by definition once the pipe closes.
// Returns "" on any failure to locate/run bezelc (never mistaken for "no
// MatchFailureException emitted" by the assertions below, which require
// both non-empty AND the marker).
std::string compileAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    // fix #4618: the stem must be unique across PROCESSES, not just within one.
    // `counter` is function-static, so every sharded worker starts it at 0 and
    // several processes raced on the same path: measured 8/8 pass single-process
    // but 4/8 fail at BEZEL_SHARD_COUNT=6, with a different failing subset each
    // run — a regression-shaped signature that is really a test-isolation bug.
    // getpid() disambiguates the shards; the counter still disambiguates calls
    // within one process. The directory also moves off fs::temp_directory_path()
    // (/tmp), which is a 16 GB tmpfs this project forbids for scratch, onto the
    // harness's own scratchBase().
    std::error_code sdec;
    fs::path dir = fs::path(bezel::test::aot_detail::scratchBase());
    fs::create_directories(dir, sdec);
    if (sdec) dir = fs::temp_directory_path();
    std::string stem = "bezel_3073_" + std::to_string(static_cast<long>(getpid())) +
                       "_" + std::to_string(counter++);
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

// Slices out the `MethodName` function body from dumped module IR text
// (from its `define ... @MethodName(...) {` line to the matching closing
// `}`), so the assertions below are scoped to exactly the function this
// fix changed rather than incidentally matching an unrelated MatchFailure
// reference elsewhere in the module. The codegen mangles method names to
// `bz$<module>.<class>$<method>` (LlvmBackend_Setup.cpp:
// functionName — `bz$` + mangledTypeName(ms->parent) + `$` + ms->name,
// where mangledTypeName is `module + "." + name`); a `bezelc -c` build
// drives `compileObject` which forces `isMainModule=false`, so even a
// `Main` method is mangled the same way (not promoted to `main`).
//
// The marker anchors on the MANGLED name's closing quote. After it we
// find the FIRST `{` (the body opener); between that `{` and the first
// `\n}` (the body closer) is the function body. This is robust across
// both zero-arg `() {` and N-arg `(<args>) {` definitions, and the minimal
// test programs below never call their own case-bearing methods (each
// method is the single reachable entry in its source file), so a literal
// name match is unambiguous in this corpus. (The static_init_atomic_1391
// test uses `() {` to disambiguate call-site vs definition for its
// per-instantiation `bz$T$ensureinit` function; we don't need that here
// because the test methods are never called by anyone.)
std::string sliceFunctionBody(const std::string& ir,
                              const std::string& moduleName,
                              const std::string& className,
                              const std::string& methodName) {
    std::string marker = "bz$" + moduleName + "." + className + "$" +
                         methodName + "\"";
    std::size_t start = ir.find(marker);
    if (start == std::string::npos) return "";
    std::size_t bodyStart = ir.find('{', start);
    if (bodyStart == std::string::npos) return "";
    std::size_t bodyEnd = ir.find("\n}", bodyStart);
    if (bodyEnd == std::string::npos) return "";
    return ir.substr(bodyStart, bodyEnd - bodyStart);
}

// The runtime marker for the MatchFailureException vtable the
// `emitThrowBuiltin` helper stores into the freshly-calloc'd exception
// object (LlvmBackend_Setup.cpp: `builtinVtable_[name] = new
// llvm::GlobalVariable(... "bvt$" + name)`). Pre-fix the dead edge is a
// plain `br label %casestmt.end` with no MatchFailure reference anywhere
// in the method; post-fix it carries an exception object whose vtable
// field stores @"bvt$MatchFailureException".
const char* kMatchFailureVtable = "@\"bvt$MatchFailureException\"";

// The Module-pinned scrutinee / return shape for each grant. The test
// method's enclosing context deliberately has only one `case` per
// program so a MatchFailure reference anywhere in the method body must
// come from this case's dead edge (no incidental source elsewhere).
struct CaseProgram {
    const char* label;        // diagnostic only
    std::string moduleName;   // source `module X` — sets the IR name prefix
    std::string className;    // enclosing class — sets the IR middle segment
    std::string methodName;   // function name — sets the IR tail segment
    std::string source;       // full source — ends with `end <Method>;`
};

// ---------------------------------------------------------------------------
// 1. ENUM (post-#3044): constants-coverage bullet of §21.7 condition (i).
// ---------------------------------------------------------------------------
// Two constants, two arms, every arm returns. No `_` catch-all. The grant
// closes the no-match fall-through by §5.4's invariant ("every producer of
// an enum value yields a declared value"), so the dead edge is statically
// unreachable — §21.7 requires it lower to a trap, not fall-through.
CaseProgram kEnumCase = {
    "enum",
    "M", "C", "Rank",
    "module M; "
    "enum Priority Low, High; end Priority; "
    "class C "
    "  public static method Rank(p: Priority): int "
    "    case p of "
    "      Low  => return 10;, "
    "      High => return 20; "
    "    end; "
    "  end Rank; "
    "end C;"
};

// ---------------------------------------------------------------------------
// 2. CHOICE (post-#2129): choice bullet of §21.7 condition (i). The case
// set is closed by declaration (§35.1), so an exhaustive match over a
// choice has no out-of-set value.
// ---------------------------------------------------------------------------
CaseProgram kChoiceCase = {
    "choice",
    "M", "C", "Main",
    "module M; "
    "choice R Ok(v: int), Error(m: int); end R; "
    "class C public static method Main(): int "
    "  var r: R := R.Ok(7); "
    "  case r of "
    "    Ok(var v)    => do return v; end, "
    "    Error(var m) => do return m; end "
    "  end; "
    "end Main; end C;"
};

// ---------------------------------------------------------------------------
// 3. BOOL (post-#2133): bool bullet of §21.7 condition (i). §4.1 — no
// conversion manufactures a third value, so true/false cover the space.
// ---------------------------------------------------------------------------
CaseProgram kBoolCase = {
    "bool",
    "M", "C", "Negate",
    "module M; "
    "class C "
    "  public static method Negate(b: bool): bool "
    "    case b of "
    "      true  => return false;, "
    "      false => return true; "
    "    end; "
    "  end Negate; "
    "end C;"
};

// ---------------------------------------------------------------------------
// 4. SEALED HIERARCHY (post-#2133): sealed-hierarchy bullet of §21.7
// condition (i). §11.7 — a `closed` class root closes the concrete-leaf
// set, so every instance is one of the declared leaves.
// ---------------------------------------------------------------------------
CaseProgram kSealedCase = {
    "sealed",
    "M", "C", "Classify",
    "module M; "
    "closed class Shape "
    "  public abstract method Tag(): int; "
    "end Shape; "
    "sealed class Circle : Shape "
    "  public override method Tag(): int => 1; "
    "  destructor end destructor; "
    "end Circle; "
    "sealed class Square : Shape "
    "  public override method Tag(): int => 2; "
    "  destructor end destructor; "
    "end Square; "
    "class C "
    "  public method Classify(s: Shape): int "
    "    case s of "
    "      var c: Circle => return 1;, "
    "      var q: Square => return 2; "
    "    end; "
    "  end Classify; "
    "end C;"
};

// ---------------------------------------------------------------------------
// 5. NULLABLE BULLET (§21.7 condition (i) last sentence): "any of these
// under the nullable bullet with its explicit `null` arm". A nullable
// scrutinee whose only out-of-non-null-set values are NULL and the
// declared constants is closed by an arm-per-nullable-value pair: an
// explicit `null` arm plus exhaustive coverage of the non-null value
// space. Verified on the same enum used in Test 1, here lifted to
// Priority? — the dead edge is statically unreachable and §21.7
// requires the lowering be a defined trap, not a fall-through.
// ---------------------------------------------------------------------------
CaseProgram kNullableEnumCase = {
    "nullable-enum",
    "M", "C", "Tag",
    "module M; "
    "enum Priority Low, High; end Priority; "
    "class C "
    "  public static method Tag(p: Priority?): int "
    "    case p of "
    "      Low  => return 10;, "
    "      High => return 20;, "
    "      null => return 0; "
    "    end; "
    "  end Tag; "
    "end C;"
};

// Generic body of the §21.7 dead-edge assertion: compile the program,
// slice out the method, assert the dead-edge vtable marker is present
// (post-fix) and absent from any pre-fix baseline. Returns true on
// success (and the caller emits a clear PASS diagnostic).
bool assertDeadEdgeTraps(const CaseProgram& cp) {
    std::string ir = compileAndCaptureIR(cp.source);
    if (ir.empty()) return false;
    std::string body = sliceFunctionBody(ir, cp.moduleName, cp.className,
                                         cp.methodName);
    if (body.empty()) return false;
    return body.find(kMatchFailureVtable) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// §21.7 / fix #3073: every closed-value-space grant closes the dead edge
// to a defined trap (MatchFailureException), not fall-through.
// ---------------------------------------------------------------------------

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           enum_constant_exhaustive_lowering_emits_match_failure_trap) {
    BEZEL_EXPECT_TRUE(assertDeadEdgeTraps(kEnumCase));
}

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           choice_constants_exhaustive_lowering_emits_match_failure_trap) {
    BEZEL_EXPECT_TRUE(assertDeadEdgeTraps(kChoiceCase));
}

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           bool_constant_exhaustive_lowering_emits_match_failure_trap) {
    BEZEL_EXPECT_TRUE(assertDeadEdgeTraps(kBoolCase));
}

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           sealed_hierarchy_exhaustive_lowering_emits_match_failure_trap) {
    BEZEL_EXPECT_TRUE(assertDeadEdgeTraps(kSealedCase));
}

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           nullable_enum_with_explicit_null_arm_lowering_emits_trap) {
    // §21.7 nullable bullet: a Priority? scrutinee whose `null` arm plus
    // exhaustive coverage of the non-null value space (Low + High) closes
    // the dead edge — verify the lowering shape matches the other four
    // bullets.
    BEZEL_EXPECT_TRUE(assertDeadEdgeTraps(kNullableEnumCase));
}

// ---------------------------------------------------------------------------
// NEGATIVE CONTROL — a NON-totality-granted statement-`case` MUST keep
// the fall-through behavior (§19.6: unmatched falls through with no
// action). The marker is NOT allowed in the method body: if the fix
// over-fires (treats every case as granted), this assertion fails.
// ---------------------------------------------------------------------------

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           nonexhaustive_int_case_keeps_fall_through_no_trap) {
    // Int scrutinee, no catch-all — NOT in §21.7's closed-value-space
    // list (only bool / choice / enum / sealed qualify; int is an open
    // value space). §19.6 fall-through must be preserved.
    CaseProgram cp = {
        "int-fall-through",
        "M", "C", "Main",
        "module M; "
        "class C public static method Main(): int "
        "  var x: int := 99; var r := 42; "
        "  case x of "
        "    1 => do r := 10; end, "
        "    2 => do r := 20; end "
        "  end; "
        "  return r; "
        "end Main; end C;"
    };
    std::string ir = compileAndCaptureIR(cp.source);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string body = sliceFunctionBody(ir, cp.moduleName, cp.className,
                                         cp.methodName);
    BEZEL_EXPECT_TRUE(!body.empty());
    // The fix MUST NOT add a MatchFailure throw here — int is not in
    // §21.7's closed-value-space list and totalityGranted stays false.
    BEZEL_EXPECT_TRUE(body.find(kMatchFailureVtable) == std::string::npos);
}

// ---------------------------------------------------------------------------
// SEMANTIC GUARD — the runtime still returns the right answer. The
// lowering is now: every arm returns its value; the dead edge is a trap
// that no conforming input can reach. These tests exercise BOTH arms to
// confirm the match still dispatches correctly (totality did not collapse
// a branch into unreachable). A regression here would catch a lowering
// shape where the post-fix trap hijacks the arm body.
// ---------------------------------------------------------------------------

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           enum_total_case_runtime_dispatches_each_arm_3073) {
    // Same totality-granted shape as the IR test above; verify both
    // arms still produce the right runtime value (Low→10, High→20).
    auto r = bezel::test::runAot(
        std::string("module M; enum Priority Low, High; end Priority; "
                    "class C "
                    "  public static method Rank(p: Priority): int "
                    "    case p of "
                    "      Low  => return 10;, "
                    "      High => return 20; "
                    "    end; "
                    "  end Rank; "
                    "  public static method Main(): int "
                    "    return Rank(Priority.Low) + Rank(Priority.High); "
                    "  end Main; "
                    "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

BEZEL_TEST(case_stmt_total_dead_edge_trap_3073,
           choice_total_case_runtime_dispatches_each_arm_3073) {
    auto r = bezel::test::runAot(
        std::string("module M; "
                    "choice R Ok(v: int), Error(m: int); end R; "
                    "class C public static method Main(): int "
                    "  var r: R := R.Ok(11); "
                    "  case r of "
                    "    Ok(var v)    => do return v; end, "
                    "    Error(var m) => do return m; end "
                    "  end; "
                    "end Main; end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}