// fix #3477 (spec §29.1 "The protocol" / §29.11 "Multiple enumeration",
// v0.283 -- CONTEXTUAL citations, see SCOPE below): every synthesized
// enumerator vtable cache in `LlvmBackend_Assign.cpp` missed, so each wrap
// site minted a fresh vtable global and LLVM uniqued the name.
//
// THE DEFECT. Eleven wrap helpers cached their vtable as
//
//     std::string vtName = prefix + "vtable";
//     auto* vtGV = module_->getGlobalVariable(vtName);
//     if (!vtGV) { ... new llvm::GlobalVariable(..., InternalLinkage, ..., vtName); }
//
// `llvm::Module::getGlobalVariable(StringRef)` forwards to
// `getGlobalVariable(Name, /*AllowInternal=*/false)` (llvm/IR/Module.h, whose
// own doc comment says "If AllowInternal is set to true, this function will
// return types that have InternalLinkage. By default, these types are not
// returned.")
// and returns null for a global with LOCAL linkage. Every vtable above is
// `InternalLinkage`, so the guard NEVER fired. The sibling
// `module_->getFunction(name)` has no such restriction, which is why the
// Dispose / MoveNext / Current / GetEnumerator halves of the same cache are
// genuinely shared and only the vtable duplicated.
//
// MEASURED on the parent commit, this program compiled with `bezelc -c`
// (twelve one-per-line rows, re-flowed into two columns here):
//
//     $ llvm-nm t.o | grep vtable
//     d bz$arrenum$i32$vtable          d bz$arrenum$i32$vtable.1
//     d bz$arrenumown$i32$vtable       d bz$arrenumown$i32$vtable.2
//     d bz$llenum$i32$vtable           d bz$llenum$i32$vtable.3
//     d bz$llenumown$i32$vtable        d bz$llenumown$i32$vtable.4
//     d bz$stackenum$i32$vtable        d bz$stackenum$i32$vtable.5
//     d bz$stackenumown$i32$vtable     d bz$stackenumown$i32$vtable.6
//
// 12 globals for 6 machinery kinds -- a 100% miss rate, one duplicate per
// additional wrap site. With the fix the `.1`..`.6` twins are gone and the
// six survive. The `.N` suffixes are LLVM uniquing a name that already
// existed: direct evidence the guard never fired.
//
// ⚠ SCOPE -- THIS IS A COMPILE-TIME / IR-IDENTITY DEFECT, NOT A BEHAVIOUR ONE,
// AND THIS TEST IS HONEST ABOUT THAT. The duplicates are semantically inert on
// the parent commit and the lane verified that rather than assuming it:
//
//   * The vtable's four function slots are filled from `getFunction(name)`
//     lookups that DO find internal functions, so every twin points at the
//     same four functions. Only slot 0 -- the self-referential type-descriptor
//     sentinel -- differs, each twin pointing at itself.
//   * Nothing compares these vtable pointers for identity. The emitted IR for
//     this program contains no `icmp` against a vtable global, and the
//     enumerator vtables are deliberately absent from `bz$vtable$table`
//     (`bz_obj_is_string`'s whitelist), whose contract states that
//     "Runtime-internal vtables — IEnumerator state machines — are never
//     stored in a plain Object slot, so omitting them from the table is sound"
//     (src/runtime/bezel_runtime_refcounting.cpp).
//   * `bz_itable_lookup` reads an itmap at vtable slot 1; these vtables hold
//     Dispose there and never reach that path.
//   * The program below prints the same `n=14` on the parent commit and on
//     this one (measured both ways), so no `runAot` row can separate them.
//
// So the OBSERVABLE is the module's global list, and a lit/FileCheck row over
// the emitted IR is the right and sufficient oracle -- not a behaviour test
// dressed up. Row (B) below still runs the program, because a fix that makes
// two wrap sites SHARE a vtable must not change what iteration produces.
//
// WHY IT IS WORTH FIXING ANYWAY. Two in-source claims are false without it.
// `wrapOwnedArrayAsIEnumerable` says its key exists "so the owning Dispose /
// GetEnumerator / vtable are created once per element type", and #3450 has
// just finished sharpening that key because a vtable whose CONTENT varies with
// something the key does not capture is a wild-free hazard. A reader who
// checks "could two element types share a vtable?" reads the guard and
// concludes no sharing is possible at all -- the opposite of true for the
// functions the vtable points at.
//
// COVERAGE. Six of the eleven edited sites are pinned here, one per machinery
// kind reachable from a callee-owned collection return:
// `ensureArrayEnumMachinery`, `wrapOwnedArrayAsIEnumerable`,
// `ensureStackEnumMachinery`, `wrapOwnedStackAsIEnumerator`,
// `ensureLinkedListEnumMachinery`, `wrapOwnedLinkedListAsIEnumerable`. The
// other five -- `wrapArrayAsIOwningEnumerator`,
// `wrapLinkedListAsIOwningEnumerator`, `ensureIEnumViewFactory`,
// `wrapDictionaryEntryArrayAsIEnumerable` and `wrapUserClassAsIEnumerable` --
// take the identical one-line edit, all five cache their FUNCTIONS through the
// same `getFunction(name)` guard, and their behaviour is covered by the gtest
// suite; no row here claims otherwise.
//
// (A) IR identity, in two passes over ONE dump of the emitted module.
// RUN: env BEZEL_DUMP_IR=1 %bezelc -c -o %t.o %s 2> %t.ll
//
// (A1) the six vtables ARE emitted -- the row that keeps (A2) from passing
//      vacuously if this program ever stops reaching these helpers.
// RUN: FileCheck %s --check-prefix=IR < %t.ll
//
// (A2) ...and NONE of them is duplicated. `$vtable.` can only occur as LLVM's
//      uniquing suffix on a name that already existed (`...$vtable.1`); the
//      legitimate names end at `vtable` and the unrelated whitelist globals
//      spell `$vtable$`. A NOT-only prefix is deliberate: `--implicit-check-not`
//      alongside CHECK-DAG does NOT flag these (measured -- it passed on the
//      parent commit with all six duplicates present), so this row is written
//      as its own pass to stay able to fail.
// RUN: FileCheck %s --check-prefix=NODUP < %t.ll
//
// (B) The program still iterates correctly with the vtables shared.
// RUN: %bezelc -o %t.exe %s
// RUN: %t.exe | FileCheck --check-prefix=EXEC %s

// IR-DAG: @"bz$arrenum$i32$vtable" = internal constant
// IR-DAG: @"bz$arrenumown$i32$vtable" = internal constant
// IR-DAG: @"bz$llenum$i32$vtable" = internal constant
// IR-DAG: @"bz$llenumown$i32$vtable" = internal constant
// IR-DAG: @"bz$stackenum$i32$vtable" = internal constant
// IR-DAG: @"bz$stackenumown$i32$vtable" = internal constant

// NODUP-NOT: $vtable.

// EXEC: n=14

module M;
public class Main
  /* Two callee-owned returns per family: the SECOND one is the wrap site the
     broken cache duplicated the vtable at. Each family reaches a different
     pair of helpers (borrowing machinery + owning wrap). */
  public static method MkArrA(): IEnumerable<int>
    var a := new Array<int>(2, 1);
    return a;
  end MkArrA;
  public static method MkArrB(): IEnumerable<int>
    var a := new Array<int>(2, 3);
    return a;
  end MkArrB;
  public static method MkStkA(): IEnumerable<int>
    var s := new Stack<int>();
    s.Push(1);
    return s;
  end MkStkA;
  public static method MkStkB(): IEnumerable<int>
    var s := new Stack<int>();
    s.Push(2);
    return s;
  end MkStkB;
  public static method MkLlA(): IEnumerable<int>
    var l := new LinkedList<int>();
    l.AddLast(1);
    return l;
  end MkLlA;
  public static method MkLlB(): IEnumerable<int>
    var l := new LinkedList<int>();
    l.AddLast(2);
    return l;
  end MkLlB;
  public static method Sum(e: IEnumerable<int>): int
    var n := 0;
    for var x in e do
      n := n + x;
    end for;
    return n;
  end Sum;
  public static method Main(): int
    using var a1 := MkArrA();
    using var a2 := MkArrB();
    using var s1 := MkStkA();
    using var s2 := MkStkB();
    using var l1 := MkLlA();
    using var l2 := MkLlB();
    /* Array<int>(size, fill): (2, 1) is two 1s and (2, 3) is two 3s, so the
       total is 2 + 6 + 1 + 2 + 1 + 2 = 14 -- asserted by the EXEC row. */
    var n := Sum(a1) + Sum(a2) + Sum(s1) + Sum(s2) + Sum(l1) + Sum(l2);
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end Main;
