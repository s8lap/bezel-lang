// fix #2205 — §28.3 (v0.226): an unbound fresh receiver temporary must be
// reclaimed after a call whose announced `owned` result is a declaration-less
// owning builtin. #1859 admitted the class/interface result case, but its
// declaration gate skipped `OwningArray<T>` and the other BuiltinRegistry
// owning heads, leaking the receiver block even though the collection result
// itself was correctly disposed by #1909.
//
// Every test uses runAotChecked: the exit code checks the observable disposal /
// value balance, while the AOT ASan/LSan leg proves there is no receiver leak,
// double-free, or use-after-free.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& source) {
    auto result = runAotChecked(source);
    return result.ran ? result.exitCode : -999;
}

// §28.3 producers table: `new Mk()` is an unbound owned temporary used only as
// the receiver. `Make` announces an owned OwningArray transfer, so the result
// cannot be a signature-level borrow of Mk and the receiver is reclaimed after
// the call. The array discard independently disposes Res. Res.n == 0 and
// Mk.n == 1 prove both disposal paths; LSan catches the old 16-byte Mk leak.
BEZEL_TEST(fresh_owned_receiver_2205,
           owning_collection_result_reclaims_receiver_28_3) {
    const std::string source =
        "module M; "
        "class Res "
        "  public static n: int := 0; "
        "  public constructor() n := n + 1; end constructor; "
        "  destructor n := n - 1; end destructor; "
        "end Res; "
        "class Mk "
        "  public static n: int := 0; "
        "  public method Make(): owned OwningArray<Res> "
        "    var a := new OwningArray<Res>(); a.Add(new Res()); return a; "
        "  end Make; "
        "  destructor n := n + 1; end destructor; "
        "end Mk; "
        "class P "
        "  public static method Main(): int "
        "    do _ := new Mk().Make(); end; "
        "    return Res.n * 10 + Mk.n; "
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(source), 1);
}

// §28.3 value-result control: the pre-existing primitive/enum/unit admission
// remains unchanged. A fresh receiver used for an `int` result is reclaimed,
// and its destructor runs exactly once.
BEZEL_TEST(fresh_owned_receiver_2205, primitive_result_control_28_3) {
    const std::string source =
        "module M; "
        "class Box "
        "  public static n: int := 0; "
        "  public method Read(): int return 7; end Read; "
        "  destructor n := n + 1; end destructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var value: int := 0; "
        "    do value := new Box().Read(); end; "
        "    return value + Box.n; "
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(source), 8);
}

// §28.3 ordinary instance-call row control: a non-`owned` class result is a
// borrow of the receiver, so the bound receiver remains the sole owner and is
// disposed exactly once. This guards #1859's class/interface behavior while the
// #2205 branch handles only announced-owned BuiltinRegistry heads.
BEZEL_TEST(fresh_owned_receiver_2205, borrowed_class_result_control_28_3) {
    const std::string source =
        "module M; "
        "class Node "
        "  public static n: int := 0; "
        "  public Value: int := 7; "
        "  public method Self(): Node return this; end Self; "
        "  destructor n := n + 1; end destructor; "
        "end Node; "
        "class P "
        "  public static method Main(): int "
        "    var value: int := 0; "
        "    do "
        "      using var node := new Node(); "
        "      value := node.Self().Value; "
        "    end; "
        "    return value + Node.n * 10; "
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(source), 17);
}

}  // namespace
