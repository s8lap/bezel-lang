// Codegen / ownership — a builtin `Try*` out-write into a PRE-DECLARED
// string recipient stored the collection's pointer with no retain, while the
// recipient's slot owns a reference that its scope end releases, so
// `d.TryGetValue(k, out v)` over-released the borrowed string ("refcount
// over-release (string)", rc 134). The inline `out var v` spelling was
// clean. Issue #8268.
//
// Spec: stdlib §4.8 Dictionary<K, V>.TryGetValue (the out is a borrow of the
// stored value) and §4.11 Stack/Queue TryPeek (`// out is borrow`) /
// TryPop / TryDequeue (the element moves out); spec §22.3 / §22.4 (out
// arguments and inline `out var`), §28.9 "Runtime-managed types" (a string
// is refcounted). Spec revision v0.355.
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the defect). runAotChecked runs
// under ASan/LSan, so a leak, a double-free and the runtime's over-release
// abort all flip `r.ran`; stdout pins the values read back.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Issue repro p13: `Dictionary<int, string>` read into a pre-declared
// `var v: string? := null`, 200x. Aborted (over-release) before the fix.
const char* kP13StringDictPredeclared =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      d[1] := \"zz\" + i.ToString();\n"
    "      tot := tot + d[1].Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Issue repro p15 (`Dictionary<int, string?>`), wrapped in a 100x loop.
const char* kP15NullableDictOnce =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      d[1] := \"a\" + (40 + d.Count + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        if i = 99 then Console.WriteLine(v ?? \"none\"); end;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Issue repro p10: the out is the only use of the stored value.
const char* kP10BorrowOnly =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Issue repro p01: the stored value is overwritten after the read; the
// recipient must still hold its own reference (no use-after-free).
const char* kP01ThenOverwrite =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      d[1] := \"zz\" + i.ToString();\n"
    "      tot := tot + (d[1] ?? \"\").Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Sibling: SortedDictionary.TryGetValue (stdlib §4.8 shape), 100x.
const char* kSortedDict =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new SortedDictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Sibling: Stack.TryPeek (`// out is borrow`, stdlib §4.11), 100x.
const char* kStackTryPeek =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var s := new Stack<string>();\n"
    "      s.Push(\"a\" + (40 + i mod 10).ToString());\n"
    "      var v: string? := null;\n"
    "      if s.TryPeek(out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Sibling: Queue.TryPeek, 100x.
const char* kQueueTryPeek =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var q := new Queue<string>();\n"
    "      q.Enqueue(\"a\" + (40 + i mod 10).ToString());\n"
    "      var v: string? := null;\n"
    "      if q.TryPeek(out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// #6936 repro 2 (the TryPeek half of that issue): a `using var` stack.
const char* kStackTryPeekUsingVar =
    "module LocS;\n"
    "public class G\n"
    "  public static method Stk(): owned Stack<string>\n"
    "    var v := new Stack<string>(); v.Push($\"s-{1}\"); v.Push($\"s-{2}\"); return v;\n"
    "  end Stk;\n"
    "end G;\n"
    "class C public static method Main(): int\n"
    "  var t := 0; var i := 0;\n"
    "  while i < 200 do\n"
    "    using var k := G.Stk(); var q: string? := null; _ := k.TryPeek(out q); t += (q ?? \"\").Length;\n"
    "    i += 1;\n"
    "  end;\n"
    "  Console.WriteLine(t);\n"
    "  return 0;\n"
    "end Main; end C;\n";

// TryPop into a recipient that already owns a runtime string: the moved
// element is owned by the slot and the displaced value is released once
// (it leaked before the fix).
const char* kStackTryPopOldValue =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var s := new Stack<string>();\n"
    "      s.Push(\"a\" + (40 + i mod 10).ToString());\n"
    "      var v: string? := \"b\" + i.ToString();\n"
    "      if s.TryPop(out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// TryDequeue hit then miss over an owned old value: the miss path nulls the
// slot and releases the moved element exactly once.
const char* kQueueTryDequeueOldValueHitMiss =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var q := new Queue<string>();\n"
    "      q.Enqueue(\"a\" + (40 + i mod 10).ToString());\n"
    "      var v: string? := \"b\" + i.ToString();\n"
    "      if q.TryDequeue(out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      if not q.TryDequeue(out v) then\n"
    "        tot := tot + (v ?? \"xy\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// TryGetValue hit then miss over a producer-initialized recipient (aborted
// before the fix).
const char* kDictOldValueHitMiss =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := \"b\" + i.ToString();\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      if not d.TryGetValue(2, out v) then\n"
    "        tot := tot + (v ?? \"xyz\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The recipient already holds the same string the dictionary stores, read
// twice: retain-new before release-old keeps it alive.
const char* kDictSelfAlias =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      var v: string? := \"a\" + (40 + i mod 10).ToString();\n"
    "      d[1] := v ?? \"\";\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The read value is copied into a longer-lived Array, then read again
// (aborted before the fix).
const char* kDictEscapeToArray =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var keep := new Array<string>();\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        keep.Add(v ?? \"\");\n"
    "      end;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot + keep.Count);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A recipient declared without an initializer.
const char* kNoInitializer =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string?;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A recipient that is written and never read still releases its reference.
const char* kUnreadRecipient =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + 1;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A field out target (§22.3 admits it): the field owns its reference.
const char* kFieldTarget =
    "module M; import System;\n"
    "class H\n"
    "  public Name: string? := null;\n"
    "end H;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      using var h := new H();\n"
    "      h.Name := \"b\" + i.ToString();\n"
    "      if d.TryGetValue(1, out h.Name) then\n"
    "        tot := tot + (h.Name ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Forwarding through an enclosing `out s: string?` parameter.
const char* kForwardedOutParam =
    "module M; import System;\n"
    "class C\n"
    "  static method Look(d: Dictionary<int, string>, out s: string?): bool\n"
    "    return d.TryGetValue(1, out s);\n"
    "  end Look;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var v: string? := null;\n"
    "      if Look(d, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// `out var` with Stack.TryPop: the moved element is owned by the inline
// recipient (it leaked before the fix; the pre-declared spelling now agrees).
const char* kOutVarStackTryPop =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var s := new Stack<string>();\n"
    "      s.Push(\"a\" + (40 + i mod 10).ToString());\n"
    "      if s.TryPop(out var v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// `out var` with Queue.TryDequeue (leaked before the fix).
const char* kOutVarQueueTryDequeue =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var s := new Queue<string>();\n"
    "      s.Enqueue(\"a\" + (40 + i mod 10).ToString());\n"
    "      if s.TryDequeue(out var v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the inline `out var` spelling the issue names as clean.
const char* kControlOutVarDict =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var s := new Dictionary<int, string>();\n"
    "      s[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      if s.TryGetValue(1, out var v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: inline `out var` with Stack.TryPeek.
const char* kControlOutVarStackPeek =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var s := new Stack<string>();\n"
    "      s.Push(\"a\" + (40 + i mod 10).ToString());\n"
    "      if s.TryPeek(out var v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: an `int` value type (no refcount involved).
const char* kControlIntValue =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, int>();\n"
    "      d[1] := 40 + i mod 10;\n"
    "      var v: int? := null;\n"
    "      if d.TryGetValue(1, out v) then\n"
    "        tot := tot + (v ?? 0);\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: a user method with an `out s: string?` parameter.
const char* kControlUserOutString =
    "module M; import System;\n"
    "class C\n"
    "  static method Get(i: int, out s: string?): bool\n"
    "    s := \"a\" + (40 + i mod 10).ToString();\n"
    "    return true;\n"
    "  end Get;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var v: string? := null;\n"
    "      if Get(i, out v) then\n"
    "        tot := tot + (v ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: an array-element out target (copy-in/copy-out bracket).
const char* kControlArrayElement =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 100 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      var a := new Array<string?>();\n"
    "      a.Add(null);\n"
    "      if d.TryGetValue(1, out a[0]) then\n"
    "        tot := tot + (a[0] ?? \"\").Length;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak, no double-free, no abort
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           p13_string_dict_predeclared_recipient_spec_28_9) {
    expectClean(kP13StringDictPredeclared, "1490\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           p15_nullable_dict_predeclared_recipient_spec_28_9) {
    expectClean(kP15NullableDictOnce, "a49\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           p10_nullable_dict_borrow_only_spec_28_9) {
    expectClean(kP10BorrowOnly, "600\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           p01_read_then_overwrite_stored_value_spec_28_9) {
    expectClean(kP01ThenOverwrite, "1490\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           sorted_dictionary_trygetvalue_predeclared_spec_28_9) {
    expectClean(kSortedDict, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           stack_trypeek_predeclared_borrow_spec_4_11) {
    expectClean(kStackTryPeek, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           queue_trypeek_predeclared_borrow_spec_4_11) {
    expectClean(kQueueTryPeek, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           stack_trypeek_using_var_stack_issue_6936_repro2_spec_4_11) {
    expectClean(kStackTryPeekUsingVar, "600\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           stack_trypop_releases_displaced_old_value_spec_28_9) {
    expectClean(kStackTryPopOldValue, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           queue_trydequeue_old_value_hit_then_miss_spec_28_9) {
    expectClean(kQueueTryDequeueOldValueHitMiss, "500\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_old_value_hit_then_miss_spec_28_9) {
    expectClean(kDictOldValueHitMiss, "600\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_recipient_already_holds_stored_string_spec_28_9) {
    expectClean(kDictSelfAlias, "600\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_recipient_copied_into_array_spec_28_9) {
    expectClean(kDictEscapeToArray, "400\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_uninitialized_recipient_spec_28_9) {
    expectClean(kNoInitializer, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_unread_recipient_spec_28_9) {
    expectClean(kUnreadRecipient, "100\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_into_string_field_spec_22_3) {
    expectClean(kFieldTarget, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           dictionary_trygetvalue_forwarded_through_out_param_spec_22_3) {
    expectClean(kForwardedOutParam, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           out_var_stack_trypop_releases_moved_element_spec_22_4) {
    expectClean(kOutVarStackTryPop, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           out_var_queue_trydequeue_releases_moved_element_spec_22_4) {
    expectClean(kOutVarQueueTryDequeue, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           control_out_var_dictionary_trygetvalue_spec_22_4) {
    expectClean(kControlOutVarDict, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           control_out_var_stack_trypeek_spec_22_4) {
    expectClean(kControlOutVarStackPeek, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           control_int_value_dictionary_predeclared_spec_4_8) {
    expectClean(kControlIntValue, "4450\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           control_user_method_out_string_predeclared_spec_22_3) {
    expectClean(kControlUserOutString, "300\n");
}

BEZEL_TEST(codegen_tryout_predeclared_string_recipient_8268,
           control_array_element_out_target_spec_22_3) {
    expectClean(kControlArrayElement, "300\n");
}
