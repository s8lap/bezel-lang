// #2195 / §28.14 v0.226: a closure passed to a plain function-typed
// parameter is call-bounded. Capturing an owned local borrows it for the call,
// but the caller still owns and must reclaim the local's backing at scope end.
// The capture environment and its shared cell must be released without making
// the owned local escape or disposing it twice.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& source) {
    auto result = runAotChecked(source);
    return result.ran ? result.exitCode : -999;
}

}  // namespace

// §28.14 / §28.9: the load-bearing regression. The plain `Apply` parameter is
// call-bounded, so the lambda's borrow of `local` cannot outlive this call. The
// Array backing and the closure cell/env must both be reclaimed. Before #2195,
// this shape reports an LSan leak (56 bytes on the canonical repro).
BEZEL_TEST(codegen_call_bounded_capture_2195,
           owned_array_capture_reclaimed_at_call) {
    const std::string source =
        "module M; "
        "class C "
        "  public static method Apply(fn: () -> int): int return fn(); end Apply; "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); "
        "    return C.Apply(lambda(): int => local[0]); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(source), 7);
}

// §28.14 control: a non-capturing callback is still a fresh closure temporary.
// Its call-bounded post-call release must reclaim the closure cell/env without
// relying on a captured-owner disposal.
BEZEL_TEST(codegen_call_bounded_capture_2195,
           non_capturing_callback_released) {
    const std::string source =
        "module M; "
        "class C "
        "  public static method Apply(fn: () -> int): int return fn(); end Apply; "
        "  public static method Main(): int "
        "    return C.Apply(lambda(): int => 7); "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(run(source), 7);
}

// §28.14 control: an owned Array with no capture follows the ordinary
// scope-end backing reclaim path, independently of closure-cell handling.
BEZEL_TEST(codegen_call_bounded_capture_2195,
           owned_array_without_capture_reclaimed) {
    const std::string source =
        "module M; class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); return local[0]; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(run(source), 7);
}

// §28.14 control: ordinary class-instance capture keeps its existing ownership
// semantics. The class object is disposed once after the call; releasing the
// call-bounded closure must not free the captured class or cause a double-free.
BEZEL_TEST(codegen_call_bounded_capture_2195,
           class_instance_capture_no_double_free) {
    const std::string source =
        "module M; "
        "class Box public V: int := 0; "
        "  public method Add(x: int): int V := V + x; return V; end Add; "
        "  destructor end; "
        "end Box; "
        "class C "
        "  public static method Apply(fn: () -> int): int return fn(); end Apply; "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    return C.Apply(lambda(): int => do box.Add(3); return box.V; end); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(source), 3);
}
