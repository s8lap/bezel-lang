// Codegen — issue #7668 (§26.4 / §14.1, v0.350): a member of a generic
// extension block (`extension Array<T>`) whose SIGNATURE never names `T`
// (`Swap01()`: no `T` parameter, `void` result) was called through the block's
// compiled i32-PLACEHOLDER template instead of a per-instantiation body,
// because the call-site "does this need a specialization?" test compared only
// the argument and return LLVM types. `this[0]`/`this[1]` then moved 4-byte
// elements out of 8-byte `string`/class slots, tearing the pointers; a later
// read crashed in bz_string_to_utf8z. §26.4: the block-level type parameter is
// in scope throughout the block; §14.1: a generic body behaves as if
// instantiated at the concrete type arguments.
//
// Matrix: T = string / class / int, crossed with an `Array<T>` receiver, an
// `IList<T>` receiver and a plain generic method (`PSwap<T>`, whose signature
// names `T` and so always specialized: the control).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kPrelude =
    "module R; "
    "class Pt public Id: int; public constructor(i: int) Id := i; end; end Pt; "
    "extension Array<T> "
    "  public method ASwap() "
    "    var t: T := this[0]; this[0] := this[1]; this[1] := t; "
    "  end ASwap; "
    "end; "
    "extension IList<T> "
    "  public method LSwap() "
    "    var t: T := this[0]; this[0] := this[1]; this[1] := t; "
    "  end LSwap; "
    "end; "
    "class U "
    "  public static method PSwap<T>(a: Array<T>) "
    "    var t: T := a[0]; a[0] := a[1]; a[1] := t; "
    "  end PSwap; "
    "end U; ";

std::string withMain(const std::string& body) {
    return std::string(kPrelude) +
           "class Program public static method Main(): int " + body +
           " return 0; end Main; end Program;";
}

}  // namespace

// The issue's own repro shape: the swap at `string` through an Array receiver.
BEZEL_TEST(generic_extension_signature_without_t_swap_7668,
           array_receiver_string_7668) {
    auto r = runAot(withMain(
        "var s: Array<string> := [\"x\", \"y\"]; s.ASwap(); "
        "Console.WriteLine(s[0]); Console.WriteLine(s[1]);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y\nx\n"));
}

// 201 swaps (odd: the pair ends exchanged) leave both strings intact. An even
// count would hide the tear, as 4-byte half swaps cancel in pairs.
BEZEL_TEST(generic_extension_signature_without_t_swap_7668,
           array_receiver_string_loop_201_7668) {
    auto r = runAot(withMain(
        "var s: Array<string> := [\"x\", \"y\"]; var i := 0; "
        "while i < 201 do s.ASwap(); i := i + 1; end; "
        "Console.WriteLine(s[0]); Console.WriteLine(s[1]);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y\nx\n"));
}

BEZEL_TEST(generic_extension_signature_without_t_swap_7668,
           array_receiver_class_7668) {
    auto r = runAot(withMain(
        "using var p1 := new Pt(1); using var p2 := new Pt(2); "
        "var a: Array<Pt> := [p1, p2]; a.ASwap(); "
        "Console.WriteLine(a[0].Id); Console.WriteLine(a[1].Id);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n1\n"));
}

BEZEL_TEST(generic_extension_signature_without_t_swap_7668,
           array_receiver_int_control_7668) {
    auto r = runAot(withMain(
        "var a: Array<int> := [1, 2]; a.ASwap(); "
        "Console.WriteLine(a[0]); Console.WriteLine(a[1]);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n1\n"));
}

BEZEL_TEST(generic_extension_signature_without_t_swap_7668,
           ilist_receiver_string_and_class_7668) {
    auto r = runAot(withMain(
        "using var p1 := new Pt(1); using var p2 := new Pt(2); "
        "var s: Array<string> := [\"x\", \"y\"]; var c: Array<Pt> := [p1, p2]; "
        "var ls: IList<string> := s; var lc: IList<Pt> := c; "
        "ls.LSwap(); lc.LSwap(); "
        "Console.WriteLine(ls[0]); Console.WriteLine(lc[0].Id);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y\n2\n"));
}

// Control: a plain generic method names `T` in its signature, so it was always
// specialized; it must keep working at all three element types.
BEZEL_TEST(generic_extension_signature_without_t_swap_7668,
           plain_generic_method_control_7668) {
    auto r = runAot(withMain(
        "using var p1 := new Pt(1); using var p2 := new Pt(2); "
        "var a: Array<int> := [1, 2]; var s: Array<string> := [\"x\", \"y\"]; "
        "var c: Array<Pt> := [p1, p2]; "
        "U.PSwap(a); U.PSwap(s); U.PSwap(c); "
        "Console.WriteLine(a[0]); Console.WriteLine(s[0]); "
        "Console.WriteLine(c[0].Id);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\ny\n2\n"));
}
