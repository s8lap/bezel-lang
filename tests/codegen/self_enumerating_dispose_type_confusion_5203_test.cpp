// Spec-derived regression test for issue #5203.
//
// §29.2 / §28.2 (v0.313): enumerator and enumerable ownership.
//
// THE DEFECT. A self-enumerating class (`SelfSeq: IEnumerable<int>,
// IEnumerator<int>`, #2384's `return this;` idiom) that has an `owned` field
// SEGVs inside `SelfSeq$Dispose` when wrapped through the interface head.
// `wrapUserClassAsIEnumerable` picks the enumerator class from the source
// class's structure (the `structurallyIsEnumerator` branch picks
// `enumTs = srcTs` whenever srcTs has MoveNext + Current — the same check that
// is correct for the two-class idiom `MyRange`/`RangeEnum` when srcTs is the
// enumerator) but then constructs the wrapper by `eagerly calling
// GetEnumerator()` (§29.11 comment at L13398) and storing the result in slot
// 1. The wrapper's `wrapDisposeFn` then calls `enumDisposeFn` (which is
// `disposerFns_[enumTs]`, i.e. `SelfSeq$Dispose`) on whatever sits in slot
// 1. For the two-class idiom, slot 1 is the separate enumerator class —
// distinct type, distinct disposer, no problem. For the SELF-enumerating
// idiom, GetEnumerator() returns `this` (SelfSeq itself), so slot 1 IS
// SelfSeq, and `SelfSeq$Dispose(SelfSeq)` is the right dispatch — BUT, if
// the source type carries an `owned` field, the wrapper's own §29.11 alias
// guard (`enumPtr == srcObj`) trips on slot 1 == slot 2 and SKIPS the
// physical reclaim, which leaves the SelfSeq object alive after the wrapper
// has freed itself. The SelfSeq is then disposed a second time by the
// §28.3 scope-end `using var` disposal of the original binding — and that
// double dispose crashes inside SelfSeq$Dispose when the owned field has
// already been cascade-released.
//
// MEASURED on r23-c28 (81afd1c4): the `owned` field variant returns a
// non-zero exit code (SEGV-shaped: native exit code 139 / -11, or an ASan
// `heap-use-after-free` / `SEGV on unknown address` inside
// `SelfSeq$Dispose`). The pre-owned-field variant in #2975's
// `self_enumerating_idiom_wrap_still_applies_29_2` was clean — the defect
// only triggers when srcTs owns something (so a §28.2 cascade is in play).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// SelfSeq: a class implementing BOTH `IEnumerable<int>` AND `IEnumerator<int>`,
// whose `GetEnumerator()` returns an OWNED-FIELD's enumerator (the
// §4931-admitted control: the receiver is `this`, but the body returns
// `this._a.GetEnumerator()`, the SAME source the #2975 fixture's `MyE` and
// `MyRange` controls use). With an `owned` field, `wrapUserClassAsIEnumerable`
// picks the enumerator class from `structurallyIsEnumerator(srcTs)` — true
// here because SelfSeq declares MoveNext + Current — so `disposerFns_[enumTs]
// == SelfSeq$Dispose` is the disposer closed over into `wrapDisposeFn`.
// `enumPtr` is the result of `SelfSeq.GetEnumerator()` — `_a.GetEnumerator()`,
// a BUILTIN `Array<int>` enumerator (different layout). `SelfSeq$Dispose` is
// then called on that builtin Array enumerator: `cmpb $0x0,0x8(%rdi)` reads
// from inside the builtin's data, sets the disposed flag in builtin's
// storage, and `DisposeCore` reads an `owned` field at `0x10(%rdi)` that
// is, by construction, the builtin's backing array pointer (interpreted as
// a SelfSeq `owned _a` slot, but pointing to a DIFFERENT array than `_a`)
// — a type confusion. The wrapper's flag-guarded public Dispose on the source
// instance never runs; the SECOND free path (`srcDisposeFn(srcObj)`,
// dispatched from the same block when `aliasesSrc == false`) re-frees the
// source instance a moment later — confirmed as an ASan
// `heap-use-after-free` inside `SelfSeq$Dispose` (the second dispose
// reads a `_a` pointer that the first cascade's `bz_array_free` already
// freed). The `return this;` shape the #2975 fixture uses does NOT trigger
// this — the wrap's `enumPtr == srcObj` guard correctly returns `this` and
// dispatches SelfSeq$Dispose on the source — but the wrap's
// `structurallyIsEnumerator(srcTs)` arm fires for every MoveNext+Current
// class regardless of what GetEnumerator's body actually returns.
const char* kSelfSeq =
    "class SelfSeq: IEnumerable<int>, IEnumerator<int> "
    "  private _i: int := 0; "
    "  private owned _a: Array<int>; "
    "  public constructor() _a := new Array<int>(1); end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return _a.GetEnumerator(); "
    "  end GetEnumerator; "
    "  public method MoveNext(): bool "
    "    _i := _i + 1; return _i <= 3; "
    "  end MoveNext; "
    "  public property Current: int get => _i; end Current; "
    "end SelfSeq; ";

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; import System.Diagnostics; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// The §28.3 scope-end dispose: wrap SelfSeq through the IEnumerable<int>
// head with a `using var`. With r23-c28's pre-fix wrapDisposeFn, the wrapper
// calls SelfSeq$Dispose on the SelfSeq at slot 1, the alias guard skips the
// slot-1 reclaim (slot 1 == slot 2), the wrapper frees its own struct, and
// then the §28.3 `using var` dispatch disposes SelfSeq a second time —
// inside SelfSeq$Dispose, the already-freed owned `_a` triggers a SEGV or
// ASan heap-use-after-free.
//
// Looping 50× amplifies: even a one-in-N race surfaces here.
BEZEL_TEST(codegen_self_enumerating_dispose_type_confusion_5203,
           using_var_with_owned_field_does_not_double_free_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog(
            "var i := 0; "
            "while i < 50 do "
            "  using var e: IEnumerable<int> := new SelfSeq(); "
            "  i := i + 1; "
            "end; return 0;",
            kSelfSeq)),
        0);
}

// Same shape, generated-code ASan: a heap-use-after-free in SelfSeq$Dispose
// (or in the owned `_a` cascade) is caught by ASan, never reaching exit 0.
BEZEL_TEST(codegen_self_enumerating_dispose_type_confusion_5203,
           using_var_with_owned_field_leak_free_under_asan_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 50 do "
            "  using var e: IEnumerable<int> := new SelfSeq(); "
            "  i := i + 1; "
            "end; return 0;",
            kSelfSeq)),
        0);
}