// Codegen: checked-disposal guards on compiler-known class methods
// (issue #627 / §28.2 v0.135 / §31.4).
//
// §28.2 v0.135: any instance member access on a disposed instance raises
// DisposedAccessException in checked-disposal mode.  Compiler-known types
// (Shared<T>, Weak<T>, Array<T> family, Dictionary<K,V> family) are lowered
// INLINE — they do not go through emitFunctionBody and therefore do not receive
// the standard entry-guard injected by emitFunctionBody.  This file verifies
// that the checked-mode guard is emitted at each compiler-known call-lowering
// site.
//
// Test groups:
//   Shared<T> methods after Dispose() in checked mode:
//     1. Shared.Get()      after Dispose → DisposedAccessException.
//     2. Shared.Clone()    after Dispose → DisposedAccessException.
//     3. Shared.Downgrade() after Dispose → DisposedAccessException.
//   Shared<T> live-handle access (must not raise):
//     4. Shared.Get() on live handle → correct inner value, no exception.
//   Array<T> methods after Dispose() in checked mode:
//     5. Array.Add()       after Dispose → DisposedAccessException.
//     6. Array.Remove()    after Dispose → DisposedAccessException.
//     7. Array.Count       after Dispose → DisposedAccessException.
//     8. Array.Contains()  after Dispose → DisposedAccessException.
//   Array<T> live-collection access (must not raise):
//     9. Array.Add() on live array → correct Count, no exception.
//   Dictionary<K,V> methods after Dispose() in checked mode:
//    10. Dictionary.Add()         after Dispose → DisposedAccessException.
//    11. Dictionary.ContainsKey() after Dispose → DisposedAccessException.
//    12. Dictionary.Remove()      after Dispose → DisposedAccessException.
//    13. Dictionary.Count         after Dispose → DisposedAccessException.
//   Dictionary<K,V> live-collection access (must not raise):
//    14. Dictionary.Add() on live dict → correct Count, no exception.
//   Default (unchecked) mode unchanged:
//    15. Correct program (no dispose then access) identical output in both modes.
//
// Spec refs (v0.135):
//   §28.2  "Use after disposal; checked disposal mode"
//   §28.13 "Shared<T> and Weak<T>"
//   §4.4   "Array<T>"
//   §4.8   "Dictionary<K,V>"
//   §31.4  "DisposedAccessException"

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

// runChecked uses checked-disposal MODE (§28.2 — DisposedAccessException on
// use-after-dispose) via the AOT harness — NOT the ASan memory net
// (runAotChecked); see support/aot_support.hpp.
int runChecked(const std::string& program) {
    auto r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}

int runDefault(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A simple class for Shared<T> tests.
const char* kCounterClass =
    "module M; "
    "class Inner "
    "  public static disposed: int := 0; "
    "  public V: int := 0; "
    "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
    "end Inner; ";

} // anonymous namespace


// ── §28.2 / §28.13: Shared.Get() after Dispose raises ───────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_shared_get_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §28.13: Get() on a disposed Shared<T> handle must raise
    // DisposedAccessException in checked mode. This exercises the inline-lowering
    // guard path (not the emitFunctionBody method-entry guard).
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Probe(kill: bool): int "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(move obj); "
        "    var view := h; "
        "    if kill then h.Dispose(); end; "
        "    try "
        "      var b := view.Get(); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 1);
}


// ── §28.2 / §28.13: Shared.Clone() after Dispose raises ─────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_shared_clone_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §28.13: Clone() on a disposed Shared<T> handle must raise
    // DisposedAccessException in checked mode.
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Probe(kill: bool): int "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(move obj); "
        "    var view := h; "
        "    if kill then h.Dispose(); end; "
        "    try "
        "      using var h2 := view.Clone(); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 1);
}


// ── §28.2 / §28.13: Shared.Downgrade() after Dispose raises ─────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_shared_downgrade_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §28.13: Downgrade() on a disposed Shared<T> handle must
    // raise DisposedAccessException in checked mode.
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Probe(kill: bool): int "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(move obj); "
        "    var view := h; "
        "    if kill then h.Dispose(); end; "
        "    try "
        "      using var w := view.Downgrade(); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 1);
}


// ── §28.2 / §28.13: live Shared handle — Get() succeeds in checked mode ──────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_shared_get_live_ok) {
    // §28.2 v0.135: guard fires only for a disposed handle. Get() on a live
    // Shared<T> handle must succeed normally in checked mode.
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 42; "
        "    using var h := new Shared<Inner>(move obj); "
        "    var b := h.Get(); "
        "    return b.V; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 42);
}


// ── §28.2 / §4.4: Array.Add() after Dispose raises ──────────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_array_add_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.4: Add() on a disposed Array<int> must raise
    // DisposedAccessException in checked mode. This covers the inline-lowered
    // array method path (not the indexer path, which was already guarded #621).
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var arr := new Array<int>(); "
        "    var view := arr; "
        "    if kill then arr.Dispose(); end; "
        "    try "
        "      view.Add(5); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.4: Array.Remove() after Dispose raises ───────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_array_remove_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.4: Remove() on a disposed Array<int> must raise
    // DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); "
        "    var view := arr; "
        "    if kill then arr.Dispose(); end; "
        "    try "
        "      view.Remove(1); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.4: Array.Count after Dispose raises ──────────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_array_count_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.4: Count on a disposed Array<int> must raise
    // DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); "
        "    var view := arr; "
        "    if kill then arr.Dispose(); end; "
        "    try "
        "      var n := view.Count; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.4: Array.Contains() after Dispose raises ─────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_array_contains_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.4: Contains() on a disposed Array<int> must raise
    // DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(3); "
        "    var view := arr; "
        "    if kill then arr.Dispose(); end; "
        "    try "
        "      var found := view.Contains(3); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.4: live Array — Add() succeeds in checked mode ───────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_array_add_live_ok) {
    // §28.2 v0.135: guard fires only for disposed. Add() on a live Array<int>
    // must succeed normally in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(7); "
        "    arr.Add(8); "
        "    return arr.Count; "
        "  end Main; "
        "end C;"), 2);
}


// ── §28.2 / §4.8: Dictionary.Add() after Dispose raises ─────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_dict_add_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.8: Add() on a disposed Dictionary<string,int> must raise
    // DisposedAccessException in checked mode. This covers the inline-lowered
    // dict method path.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var d := new Dictionary<string,int>(); "
        "    var view := d; "
        "    if kill then d.Dispose(); end; "
        "    try "
        "      view.Add(\"k\", 1); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.8: Dictionary.ContainsKey() after Dispose raises ─────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_dict_containskey_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.8: ContainsKey() on a disposed Dictionary<string,int>
    // must raise DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var d := new Dictionary<string,int>(); "
        "    d.Add(\"k\", 1); "
        "    var view := d; "
        "    if kill then d.Dispose(); end; "
        "    try "
        "      var found := view.ContainsKey(\"k\"); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.8: Dictionary.Remove() after Dispose raises ──────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_dict_remove_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.8: Remove() on a disposed Dictionary<string,int>
    // must raise DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var d := new Dictionary<string,int>(); "
        "    d.Add(\"k\", 1); "
        "    var view := d; "
        "    if kill then d.Dispose(); end; "
        "    try "
        "      var removed := view.Remove(\"k\"); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.8: Dictionary.Count after Dispose raises ─────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_dict_count_after_dispose_raises) {
    // Re-cut by #7435 (§28.2, v0.357): a use the explicit disposal DOMINATES is
    // a compile error, so the checked-mode backstop is observed through an
    // alias whose source has a CONDITIONAL kill ("A holder whose source was
    // disposed on only some paths to the use ... is not rejected: a use through
    // it raises `DisposedAccessException` in checked disposal mode").
    // §28.2 v0.135 / §4.8: Count on a disposed Dictionary<string,int>
    // must raise DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Probe(kill: bool): int "
        "    using var d := new Dictionary<string,int>(); "
        "    d.Add(\"x\", 10); "
        "    var view := d; "
        "    if kill then d.Dispose(); end; "
        "    try "
        "      var n := view.Count; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Probe; "
        "  public static method Main(): int return Probe(true); end Main; "
        "end C;"), 1);
}


// ── §28.2 / §4.8: live Dictionary — Add() succeeds in checked mode ───────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_dict_add_live_ok) {
    // §28.2 v0.135: guard fires only for disposed. Add() on a live
    // Dictionary<string,int> must succeed normally in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string,int>(); "
        "    d.Add(\"a\", 1); "
        "    d.Add(\"b\", 2); "
        "    return d.Count; "
        "  end Main; "
        "end C;"), 2);
}


// ── §28.2: correct program identical output in both modes ────────────────────

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_correct_prog_checked) {
    // §28.2 v0.135: a program that never uses a disposed handle must produce
    // the same result in checked mode as in default mode.
    const std::string prog =
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr.Add(20); "
        "    return arr.Count; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 2);
}

BEZEL_TEST(codegen_checked_disposal_627, spec_28_2_correct_prog_default) {
    // §28.2 v0.135: same correct program in default (unchecked) mode.
    const std::string prog =
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr.Add(20); "
        "    return arr.Count; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runDefault(prog), 2);
}
