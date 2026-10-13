// Regression pins for issue #6057 (spec §27.6 "Method references", §8.6
// "Boxing", §28.9 "Runtime-managed types"; all v0.338).
//
// `var face: IFace := value;` over a RECORD mints a §8.6 box the local owns.
// A method reference over that local (`face.GetName`, no call) captures the
// box pointer as its receiver. The escape walker classed that capture as an
// escape, which barred the local from its scope-end release — and the
// function object only BORROWED its receiver, so nobody released the box:
// 24 bytes per evaluation (1200 B / 50 allocations on the issue's repro).
//
// The fix gives the function object its own reference (retain at formation,
// released by its env disposer at its last drop) and lets the local keep its
// ordinary release. The box then lives exactly as long as the longer-lived
// of the two, so every row asserts BOTH directions on the checked oracle:
// no leak, and no early free when the function object outlives the local.
//
// Each program loops (a one-shot 24 B leak can hide behind a stale pointer;
// the pre-existing one-shot row for this shape,
// codegen_record_via_interface_box_release_3229.
// record_via_interface_method_group_ref_stays_escaped_spec_27_6, passed
// while the leak was live) and prints its result (exit codes truncate).

#include <string>

#include "support/aot_support.hpp"          // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kDecls =
    "module M6057; "
    "interface IFace "
    "  method GetName(): int; "
    "  method Plus(x: int): int; "
    "end IFace; "
    "record R(N: int): IFace "
    "  public method GetName(): int return N; end GetName; "
    "  public method Plus(x: int): int return N + x; end Plus; "
    "end R; ";

constexpr const char* kMain =
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 50 do "
    "      sink := sink + One(i); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// The issue's repro: one reference, invoked, both die at scope end.
BEZEL_TEST(method_ref_box_receiver_release_spec_27_6_6057,
          issue_repro_bound_reference_over_boxed_record_is_reclaimed_27_6) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method One(i: int): int "
        "    var value := new R(13); "
        "    var face: IFace := value; "
        "    var bound: () -> int := face.GetName; "
        "    return bound(); "
        "  end One; " + kMain;
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("650\n"));
}

// Several references over ONE box — two members, a reference per loop
// iteration, a reference on one branch only, and two that compare equal
// (§27.7.1: same method, same receiver). Each owns one reference; the count
// balances whatever their number, and the local still reads the box after
// them.
BEZEL_TEST(method_ref_box_receiver_release_spec_27_6_6057,
          many_references_over_one_box_balance_27_6) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method One(i: int): int "
        "    var value := new R(i); "
        "    var face: IFace := value; "
        "    var b1: () -> int := face.GetName; "
        "    var b2: (int) -> int := face.Plus; "
        "    var total := b1() + b2(1); "
        "    var j := 0; "
        "    while j < 3 do "
        "      var b: () -> int := face.GetName; "
        "      total := total + b(); "
        "      j := j + 1; "
        "    end; "
        "    if i mod 2 = 0 then "
        "      var e: () -> int := face.GetName; "
        "      if e = b1 then total := total + 1000; end; "
        "    end; "
        "    return total + face.GetName(); "
        "  end One; " + kMain;
    // per i: i + (i+1) + 3i + i = 6i + 1; sum over 0..49 = 6*1225 + 50 = 7400;
    // plus 1000 on each of the 25 even i.
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("32400\n"));
}

// The function object OUTLIVES the local: returned from the scope that owns
// the box, and passed straight to a callee. The local's scope-end release
// must not free the box under it (a use-after-free under ASan), and the box
// must still be reclaimed when the function object dies.
BEZEL_TEST(method_ref_box_receiver_release_spec_27_6_6057,
          reference_outliving_the_local_keeps_the_box_alive_27_6) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method Make(n: int): () -> int "
        "    var value := new R(n); "
        "    var face: IFace := value; "
        "    return face.GetName; "
        "  end Make; "
        "  public static method Call(f: () -> int): int => f(); "
        "  public static method Direct(n: int): int "
        "    var value := new R(n); "
        "    var face: IFace := value; "
        "    return C.Call(face.GetName); "
        "  end Direct; "
        "  public static method One(i: int): int "
        "    var f: () -> int := C.Make(i); "
        "    return f() + C.Direct(i); "
        "  end One; " + kMain;
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2450\n"));
}
