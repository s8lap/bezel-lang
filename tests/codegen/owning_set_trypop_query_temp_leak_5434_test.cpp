// fix #5434 (§28.3 "Local variables, ownership, and borrows" / stdlib §4.10,
// v0.315): OwningSet<T>.TryPop leaks 16 B on an unbound (inline) query
// temporary.
//
// ROOT CAUSE. The TryPop handler (LlvmBackend_Calls.cpp, the
// `m == "TryPop" && call.args.size() == 2` branch) lowers the query argument
// via `lowerExpr` but never emits disposal for it. `TryPop(query: T, out owned
// removed: T?)` takes its query as a BORROW — stdlib §4.10 declares
// `Pop(query: T): owned T` with the inline comment "borrow-taking query", and
// `TryPop`'s own `query: T` carries no `owned` marker — so the callee never
// assumes the +1 and the CALLER keeps sole ownership. An unbound owned
// temporary in that position (`new R(1)` written inline at the call) is a
// §28.3 non-owned argument with no binding to carry the obligation, and
// nothing else reclaims it: pre-fix LSan reports 16 B leaked per call.
//
// THE PREDICATE IS LOAD-BEARING. The fix disposes the query gated on
// `isFreshOwnedClassProducer`, the discriminant the #1881 Add/Push/Enqueue,
// #1039/#1053 receiver-temp and #2204 Insert fixes already use. A query that is
// NOT a fresh producer is a pre-declared OWNING LOCAL, whose own scope-end
// disposal is already the single owner; disposing it here as well frees it
// early and the scope-end disposal then reads freed memory. #5434's first
// attempt was reverted for exactly this, and the control row below is what
// pins it.
//
// ORACLE SHAPE. `runAotChecked` runs the generated program under ASan with
// LSan, so `.ran` is false on ANY sanitizer finding. Every row therefore
// asserts `.ran` EXPLICITLY: a leak-only oracle cannot see a use-after-free or
// a double-free, so `!ran && leaked` is asserted too — that keeps a soundness
// regression from being silently absorbed by a row that only asks "did it
// leak?". The previous revision of this file asked only the leak question and
// returned `false` (i.e. "did not leak") for a compile rejection or an infra
// failure, which is a vacuous pass.
//
// The rows loop 200x INSIDE the generated program (so one compile produces 200
// leaked temporaries); the C++ side runs each program ONCE. An outer C++ loop
// would multiply full AOT compiles without adding any signal.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

struct Outcome {
    bool ran = false;      // true == no sanitizer finding at all
    bool leaked = false;   // ran == false AND the finding was a leak
    int exitCode = -1;     // the program's own value channel, when it ran
};

Outcome runLeakProbe(const std::string& program) {
    auto r = runAotChecked(program);
    Outcome o;
    o.ran = r.ran;
    if (o.ran) {
        o.exitCode = r.exitCode;
    } else {
        static const std::regex kLeak(
            R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
        for (const auto& d : r.diagnostics.diagnostics()) {
            std::smatch m;
            if (std::regex_search(d.message, m, kLeak)) {
                o.leaked = true;
                break;
            }
        }
    }
    return o;
}

// Post-fix every row expects a fully clean run. Pre-fix the leak row fails on
// the final assertion (`ran` is false because LSan fired); a UAF or double
// free fails the FIRST one, so the two defect classes never read alike.
void expectClean(const Outcome& o) {
    if (!o.ran) {
        BEZEL_EXPECT_TRUE(o.leaked);  // any OTHER finding is a soundness bug
    }
    BEZEL_EXPECT_TRUE(o.ran);
}

const char* kRClass =
    "class R "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is R then var r := other as R; return r!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; end R; ";

}  // namespace

// --- Row 1: the repro. Unbound inline query temporary leaks 16 B per call ---
// Pre-fix: `out using var b11` + `new R(1)` per iteration; LSan reports the
// accumulated temporaries -> `ran` false -> row FAILS. Post-fix: clean.
BEZEL_TEST(owning_set_trypop_query_temp_leak_5434,
           minimal_repro_looped_16_byte_leak) {
    const std::string src =
        std::string("module M; ") + kRClass +
        "class Program "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); "
        "    oset.Add(move (new R(1))); "
        "    for var i in 0..<200 do "
        "      oset.TryPop(new R(1), out using var b11); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program;";

    expectClean(runLeakProbe(src));
}

// --- Row 2 (CONTROL / UAF): a pre-declared query local is NOT a fresh
// producer. Its own scope-end disposal is already the single owner, so the
// handler must not free it too. This row is the one an unconditional
// `emitDisposeAndReclaim(query)` turns into
//   AddressSanitizer: heap-use-after-free ... READ of size 8
// and it is why #5434's first attempt was reverted. It is clean both pre-fix
// (nothing is disposed) and post-fix (the predicate excludes it).
BEZEL_TEST(owning_set_trypop_query_temp_leak_5434,
           control_predeclared_query_local_no_double_free) {
    const std::string src =
        std::string("module M; ") + kRClass +
        "class Program "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); "
        "    oset.Add(move (new R(1))); "
        "    var query := new R(1); "
        "    for var i in 0..<200 do "
        "      oset.TryPop(query, out using var b11); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program;";

    expectClean(runLeakProbe(src));
}

// --- Row 3 (CONTROL / found path): the disposal must not disturb the FOUND
// path, where `bz_array_remove_at` transfers the element to the out param and
// `removed` becomes its sole owner. Pins the observable: the first TryPop
// reports true and yields X == 1, the second reports false because the
// element is gone. Returns 0 only when all three hold, so a fix that disposed
// the removed element — or the query — on the found path cannot pass.
// `existing` (an element already stored in the collection) and the freshly
// minted `new R(...)` query are distinct allocations, so the aliasing the
// runtime-pointer-equality guard defends against never arises here.
BEZEL_TEST(owning_set_trypop_query_temp_leak_5434,
           control_found_path_transfers_element_to_out_param) {
    const std::string src =
        std::string("module M; ") + kRClass +
        "class Program "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); "
        "    oset.Add(move (new R(1))); "
        "    var first := oset.TryPop(new R(1), out using var b11); "
        "    if not first then return 1; end; "
        "    if b11 is not R then return 2; end; "
        "    var got := (b11 as R)!; "
        "    if got.X <> 1 then return 3; end; "
        "    var second := oset.TryPop(new R(1), out using var b12); "
        "    if second then return 4; end; "
        "    if b12 is not null then return 5; end; "
        "    return 0; "
        "  end Main; "
        "end Program;";

    auto o = runLeakProbe(src);
    // No sanitizer finding: the removed element and the query temporary are
    // each disposed exactly once.
    expectClean(o);
    // The program's own value channel: 0 == all five observable checks held.
    BEZEL_EXPECT_EQ(0, o.exitCode);
}