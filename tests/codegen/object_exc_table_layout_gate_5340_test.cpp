// §31.4 / §33.3 / §8.1 / §28.9 (issue #5340 follow-up): the published built-in
// EXCEPTION tables must not advertise an object that does not have the §31.4
// exception-instance layout.
//
// THE DEFECT. Two tables gate every §31.4 field read the runtime performs:
// `bz$exc$vtable$table` (builtinExcVtableTablePtr) and `bz$exc$desc$table`
// (builtinExcDescTablePtr). Both are built by iterating `builtinVtable_`, which
// used to BE exactly "the built-in exceptions" — until #5340 added `Object` to
// it (setupBuiltinObject builds `bvt$Object`, the §2.8.3 root class). From then
// on the two tables published `bvt$Object` / `btd$Object` as though `Object`
// were an exception.
//
// That is a LAYOUT claim, and it is false. `bz_exc_reclaim_causechain`
// (src/runtime/bezel_runtime_itable.cpp) matches an object's vtable against
// `bz$exc$vtable$table` and, on a hit, reads the CauseChain snapshot at
// BZ_EXC_CAUSE_OFFSET = 24. A `new Object()` instance is EIGHT bytes —
// lowerNew's `calloc(1, 8)`, the vtable pointer and nothing else, because §8.1
// gives Object no fields and no base. So that read is 16 bytes past the end of
// the allocation, and the runtime dereferences whatever heap bytes it finds
// there.
//
// IT IS #5437 THAT MADE IT REACHABLE. Before #5437 nothing reclaimed an
// `Object` instance, so the stale table entry was latent. #5437 added the
// §28.9 reclaim for `new Object()` at its holder's release point (correct in
// itself — §28.9's runtime-managed list is exactly string / Type / boxed value
// types / function values, and `Object` is on none of them), which routes every
// `Object` disposal through emitReclaimBlock and so through this table.
// MEASURED consequence, gate shard codegen/0 at 2ca2b689:
// `owning_array_object_literal_5340.plain_new_object_local_tostring` SEGVs.
//
// WHY THE ORACLE IS `runAot` AND NOT THE SANITIZED ONE. This is the point of
// the file, and it is why the defect survived #5437's own six rows. Those rows
// use the ASan oracle, and ASan MASKS the fault: under an ASan allocator the
// bytes at obj+24 are redzone bytes that read back as 0, `bz_exc_reclaim_causechain`
// takes its `if (!chain) return;` early exit, and the program finishes clean.
// On a plain allocator they are live heap bytes — MEASURED, `var o := new
// Object(); Console.WriteLine(o.ToString());` dies with SIGSEGV, backtrace
// frame #0 inside bz_exc_reclaim_causechain at the `mov 0x18(%rdi),%rbx`
// (offset 24), called from main. So the ASan leg of #5437 was green over a real
// out-of-bounds read, and every row here is deliberately on the strict
// non-sanitized oracle.
//
// Each row asserts the PROGRAM RAN AND EXITED CLEANLY (a nonzero-exit or
// non-running program fails both). No row asserts anything about leaks: the
// free is #5437's and is correct; what is under test is that reaching it does
// not read out of bounds.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The issue's own repro, verbatim: a bare `new Object()` local whose ToString
// is read (§8.5 class default, "System.Object") and which is then reclaimed at
// scope end. Pre-fix: SIGSEGV (exit 139). Post-fix: 0.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §31.4 / §8.1 — the gate-FAIL row's own program. The reclaim fires at the end
// of Main, so the whole file is about what that reclaim may read.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           plain_new_object_local_reclaim_reads_no_exc_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var o := new Object();"
        "        Console.WriteLine(o.ToString());"
        "        return 0;"
        "    end Main;"
        "end Pgm;"), 0);
}

// §28.9 / §31.4 — the EXPLICIT `: Object` annotation. Same allocation, same
// 8-byte instance, same reclaim; only the spelling of the declared type
// differs, so both spellings must reach the same safe release point.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           explicitly_typed_object_local_reclaim_reads_no_exc_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var o: Object := new Object();"
        "        Console.WriteLine(o.ToString());"
        "        return 0;"
        "    end Main;"
        "end Pgm;"), 0);
}

// §28.10 / §31.4 — the `using var` spelling (#5437's primary shape). Scoped
// disposal runs the same emitDisposals arm, so it is the same reclaim.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           using_var_object_reclaim_reads_no_exc_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        using var o := new Object();"
        "        Console.WriteLine(o.ToString());"
        "        return 0;"
        "    end Main;"
        "end Pgm;"), 0);
}

// §28.3 / §31.4 — the UNWIND edge. `emitDisposals` runs the objectReclaim arm on
// both edges (normal scope end and the exception path), and the reclaim reads
// the same §31.4-gated offsets either way. This row throws AFTER the Object is
// taken, so the disposal that fires is the unwind one — a distinct emission
// path from the rows above, and the one a program hits when an Object is held
// across a call that raises. The catch swallows the exception and returns 0.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           object_reclaim_on_unwind_edge_reads_no_exc_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        try"
        "            using var o := new Object();"
        "            Console.WriteLine(o.ToString());"
        "            throw new InvalidOperationException(\"boom\");"
        "        catch var e: Exception => do"
        "            return 0;"
        "        end;"
        "        end;"
        "    end Main;"
        "end Pgm;"), 0);
}

// §31.4 / §8.1 / §28.9 — MANY Object reclaims in one run. Each is an
// independent 8-byte allocation with its own reclaim, so a single row measures
// one fault and this one exercises the path 200 times over — the amplification
// discipline for a defect whose symptom is one bad read per release point.
// The observer is `ToString().Length` ("System.Object" is 13 chars, so the
// expected exit is 200 × 13 mod 256 = 40): a real read of the object, so
// nothing is optimized away, and an exit code that is the program's own
// arithmetic rather than a bare `return 0`.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           many_object_reclaims_read_no_exc_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var sink := 0;"
        "        var j := 0;"
        "        while j < 200 do"
        "            var o := new Object();"
        "            sink := sink + o.ToString().Length;"
        "            j := j + 1;"
        "        end;"
        "        return sink mod 256;"
        "    end Main;"
        "end Pgm;"), 40);
}

// §31.4 / §33.3 — THE CONTROL THAT ATTRIBUTES THE FIX. A built-in EXCEPTION
// must keep being recognised as one: it really does carry message@8 and
// CauseChain@24, and `bz_unhandled_exception` / the reclaim read those offsets
// on it. This row is what makes the table gate a statement about LAYOUT rather
// than a blanket "empty the table" — were the gate too wide, the exception path
// would stop recognising its own types and the §33.3 report would degrade.
// The program throws an unhandled built-in exception; bz_unhandled_exception
// calls exit(70) (§33.3), so 70 is the expected exit and anything else means
// the vtable table no longer matched.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           builtin_exception_still_recognised_by_exc_vtable_table) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var s := \"boom\";"
        "        throw new InvalidOperationException(s);"
        "    end Main;"
        "end Pgm;"), 70);
}

// §8.6 / §31.4 — a §8.6 BOX in an `Object` slot. #5437's own soundness bound:
// the box is runtime-managed and reclaimed by its own rule, and the objectReclaim
// arm's runtime vtable-identity guard must leave it alone. Pre-existing and
// unchanged by the table gate, but it is the row that would go red if the gate
// disturbed the box path — so it belongs beside the others rather than only in
// #5437's file.
BEZEL_TEST(object_exc_table_layout_gate_5340,
           boxed_value_in_object_slot_unaffected_by_gate) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var j := 7;"
        "        var o: Object := j;"
        "        Console.WriteLine(o.ToString());"
        "        return 0;"
        "    end Main;"
        "end Pgm;"), 0);
}