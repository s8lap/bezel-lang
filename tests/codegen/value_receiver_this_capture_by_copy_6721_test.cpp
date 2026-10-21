// Regression tests for #6721: a closure formed in a copyable value kind's
// member (or constructor) body captures the receiver value BY COPY.
//
// Spec (v0.340), §9.4 *The receiver value is fixed at the call*:
//   "A closure formed in the body — a lambda that names `this`, explicitly or
//    through a bare member read resolving to the receiver, or a bound method
//    reference over it (§27.6) — captures the fixed receiver value by copy,
//    taken at the closure's formation into its heap capture cell (§27.5)"
//   "The capture contributes to the closure's source set exactly what a
//    captured local of the receiver's type contributes ...: nothing where the
//    type holds no borrows (§3.6), so such a closure is capture-free and goes
//    wherever a capture-free function value goes — returned, stored, passed to
//    a set-erasing position"
// and §22.7 rule 3: "In a value kind's constructor body the capture is
// instead a copy of the value under construction (§9.4), so it observes at
// capture: ... the closure holding the value as of its formation."
//
// Before the fix the `this` capture stored the receiver's ADDRESS — a stack
// slot or a full-expression temporary bounded by the call — so a closure that
// outlived the activation read a dead frame (garbage output, ASan
// stack-use-after-return). The copy lives in a heap cell the closure's env
// owns: its string members are retained at the capture and released, with the
// cell, by the env destructor.
//
// Rows run under runAotChecked (checked disposal + generated-code ASan + leak
// check) unless the row says why it cannot; leak-relevant rows loop 200x.
//
// per §9.4, §22.7, §27.5, §28.3 (v0.340)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// The program's stdout, or "<did-not-run>" when it was rejected, crashed,
// exited non-zero or (checked) leaked / tripped ASan.
std::string stdoutChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
std::string stdoutPlain(const std::string& program) {
    auto r = runAot(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace


// #6634's own record repro (p4). `p` is `MakeFrom`'s local; the closure
// returned through `Maker` outlives both activations, and `Clobber` reuses the
// dead frames before the call. Before the fix the closure read the receiver
// through its stack address (garbage, stack-use-after-return under ASan).
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           record_this_closure_returned_reads_copy_spec_9_4_6634_p4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Pt(x: int) "
        "  public method Maker(): () -> int "
        "    return lambda(): int => this.x; "
        "  end Maker; "
        "end Pt; "
        "public class Program "
        "  private static method Clobber(a: int, b: int, c: int, d: int): int "
        "    var p1 := a * 3 + b; "
        "    var p2 := c * 5 + d; "
        "    var p3 := p1 + p2; "
        "    return p3 + a + b + c + d; "
        "  end Clobber; "
        "  private static method MakeFrom(): () -> int "
        "    var p := new Pt(7); "
        "    return p.Maker(); "
        "  end MakeFrom; "
        "  public static method Main() "
        "    var f := MakeFrom(); "
        "    Console.WriteLine(Clobber(111, 222, 333, 444)); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;"),
        "3774\n7\n");
}

// A closure with a parameter, over a TEMPORARY receiver (`new Pt(7)` is a
// full-expression temporary, K8): both the bound-then-called and the
// called-in-place spellings read the copy.
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           record_this_adder_returned_from_temporary_receiver_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Pt(X: int) "
        "  public method Adder(): (int) -> int "
        "    return lambda(d: int): int => this.X + d; "
        "  end Adder; "
        "end Pt; "
        "public class Program "
        "  private static method Clobber(a: int, b: int, c: int, d: int): int "
        "    var p1 := a * 3 + b; "
        "    var p2 := c * 5 + d; "
        "    var p3 := p1 + p2; "
        "    return p3 + a + b + c + d; "
        "  end Clobber; "
        "  public static method Main() "
        "    var f := new Pt(7).Adder(); "
        "    Console.WriteLine(Clobber(111, 222, 333, 444)); "
        "    Console.WriteLine(f(5)); "
        "    Console.WriteLine(new Pt(7).Adder()(5)); "
        "  end Main; "
        "end Program;"),
        "3774\n12\n12\n");
}

// The copy owns its string members: retained at the capture, released by
// the closure's env destructor. 200 closures over one heap string, each
// outliving the temporary receiver it copied; an unbalanced retain/release
// pair either frees `s` early (ASan: use-after-free on a later iteration or on
// the final `WriteLine(s)`) or leaks it (LSan).
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           record_string_member_closure_returned_200x_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Nm(S: string) "
        "  public method Len(): () -> string "
        "    return lambda(): string => S; "
        "  end Len; "
        "end Nm; "
        "public class Program "
        "  private static method Mk(s: string): () -> string "
        "    return new Nm(s).Len(); "
        "  end Mk; "
        "  public static method Main() "
        "    var s := \"n\" + 5.ToString(); "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := Mk(s); "
        "      acc := acc + f().Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(s); "
        "  end Main; "
        "end Program;"),
        "400\nn5\n");
}

// The non-escaping case: the closure is bound to a local and invoked in the
// member body. The copy cell is freed when the closure is released (200
// iterations; a leaked cell is 200 LSan allocations).
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           record_local_closure_invoked_in_body_200x_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Pt(X: int, Name: string) "
        "  public method Sum(d: int): int "
        "    var f := lambda(k: int): int => this.X + k + Name.Length; "
        "    return f(d); "
        "  end Sum; "
        "end Pt; "
        "public class Program "
        "  public static method Main() "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var p := new Pt(i, \"nm\"); "
        "      acc := acc + p.Sum(1); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "  end Main; "
        "end Program;"),
        "20500\n");
}

// A lambda formed inside a lambda's body captures `this` from the outer
// closure's copy, so it copies the copy; both outlive the receiver.
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           nested_lambda_copies_the_copy_200x_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Nm(S: string, K: int) "
        "  public method Outer(): () -> () -> int "
        "    return lambda(): () -> int => lambda(): int => S.Length + K; "
        "  end Outer; "
        "end Nm; "
        "public class Program "
        "  private static method Mk(s: string): () -> () -> int "
        "    return new Nm(s, 3).Outer(); "
        "  end Mk; "
        "  public static method Main() "
        "    var s := \"n\" + 5.ToString(); "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var g := Mk(s); "
        "      var f := g(); "
        "      acc := acc + f(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "  end Main; "
        "end Program;"),
        "1000\n");
}

// §9.4: "For a generic value kind both counts are read at each
// instantiation (§3.6 rule 2)". At `Bx<string>` the receiver holds no borrow,
// so the closure is capture-free; the copy is laid out and its strings
// retained under that instantiation's substitution.
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           generic_record_closure_returned_per_instantiation_spec_9_4_3_6) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Bx<T>(V: T, Tag: string) "
        "  public method Getter(): () -> string "
        "    return lambda(): string => this.Tag; "
        "  end Getter; "
        "end Bx; "
        "public class Program "
        "  private static method Mk(s: string): () -> string "
        "    return new Bx<string>(s, s).Getter(); "
        "  end Mk; "
        "  public static method Main() "
        "    var s := \"n\" + 5.ToString(); "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := Mk(s); "
        "      acc := acc + f().Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(s); "
        "  end Main; "
        "end Program;"),
        "400\nn5\n");
}

// The same at a value-type instantiation, over a local receiver whose frame
// is gone before the call.
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           generic_record_int_instantiation_closure_returned_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public record Bx<T>(V: T) "
        "  public method Getter(): () -> T "
        "    return lambda(): T => this.V; "
        "  end Getter; "
        "end Bx; "
        "public class Program "
        "  private static method Clobber(a: int, b: int, c: int, d: int): int "
        "    var p1 := a * 3 + b; "
        "    var p2 := c * 5 + d; "
        "    var p3 := p1 + p2; "
        "    return p3 + a + b + c + d; "
        "  end Clobber; "
        "  private static method MakeFrom(): () -> int "
        "    var b := new Bx<int>(9); "
        "    return b.Getter(); "
        "  end MakeFrom; "
        "  public static method Main() "
        "    var f := MakeFrom(); "
        "    Console.WriteLine(Clobber(111, 222, 333, 444)); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;"),
        "3774\n9\n");
}

// A copyable (value-like) choice: "a copyable choice ... receiver" is fixed
// at the call and captured by copy. The closure returns the copied value
// itself; the caller matches it after the receiver's frame is gone.
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           choice_this_closure_returned_reads_copy_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Maker(): () -> Tag "
        "    return lambda(): Tag => this; "
        "  end Maker; "
        "end Tag; "
        "public class Program "
        "  private static method Clobber(a: int, b: int, c: int, d: int): int "
        "    var p1 := a * 3 + b; "
        "    var p2 := c * 5 + d; "
        "    var p3 := p1 + p2; "
        "    return p3 + a + b + c + d; "
        "  end Clobber; "
        "  private static method MakeFrom(): () -> Tag "
        "    var t := Tag.A(7); "
        "    return t.Maker(); "
        "  end MakeFrom; "
        "  public static method Main() "
        "    var f := MakeFrom(); "
        "    Console.WriteLine(Clobber(111, 222, 333, 444)); "
        "    case f() of "
        "      A(n) => Console.WriteLine(n), "
        "      B => Console.WriteLine(\"B\") "
        "    end case; "
        "  end Main; "
        "end Program;"),
        "3774\n7\n");
}

// The choice copy owns its active case's string payload: the tag-switched
// retain at the capture, the matching release in the env destructor.
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           choice_string_payload_closure_returned_200x_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public choice Tg "
        "  A(s: string), "
        "  B; "
        "  public method Maker(): () -> Tg "
        "    return lambda(): Tg => this; "
        "  end Maker; "
        "end Tg; "
        "public class Program "
        "  private static method Mk(s: string): () -> Tg "
        "    var t := Tg.A(s); "
        "    return t.Maker(); "
        "  end Mk; "
        "  public static method Main() "
        "    var s := \"n\" + 5.ToString(); "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := Mk(s); "
        "      case f() of "
        "        A(x) => acc := acc + x.Length, "
        "        B => acc := acc - 1 "
        "      end case; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(s); "
        "  end Main; "
        "end Program;"),
        "400\nn5\n");
}

// §9.4's move-only receiver: the capture is a borrow of the in-place storage
// bounded by the call, so a closure invoked within the body is admitted
// (the escape is the ownership rejection pinned in
// tests/ownership/move_only_receiver_closure_escape_6721_test.cpp).
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           owning_choice_closure_invoked_in_body_accepted_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class Res "
        "  public N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "public choice Own "
        "  A(owned r: Res), "
        "  B; "
        "  public method Count(): int => (case this of A(r) => r.N, B => -1); "
        "  public method Twice(): int "
        "    var f := lambda(): int => Count() * 2; "
        "    return f(); "
        "  end Twice; "
        "end Own; "
        "public class Program "
        "  public static method Main() "
        "    using var o := Own.A(new Res(5)); "
        "    Console.WriteLine(o.Twice()); "
        "  end Main; "
        "end Program;"),
        "10\n");
}

// §22.7 rule 3 (v0.340): "the closure holding the value as of its
// formation" — `X` is an `int` (defaultable, not a field §7.9 requires to be
// explicitly initialized), so rule 1 admits the capture before `X := x`, and
// the copy holds the default 0, not the later 7. runAot: a lambda formed in a
// constructor body leaks its closure (#6766).
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           record_ctor_capture_holds_value_as_of_formation_spec_22_7_rule_3) {
    BEZEL_EXPECT_EQ(stdoutPlain(
        "module M; "
        "public record P "
        "  public readonly property X: int; "
        "  public readonly property Y: int; "
        "  public constructor(x: int) "
        "    var f := lambda(): int => this.X; "
        "    X := x; "
        "    Y := f(); "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(new P(7).Y); "
        "  end Main; "
        "end Program;"),
        "0\n");
}

// A borrow-free receiver's closure is capture-free and "goes wherever a
// capture-free function value goes — returned, stored": here into a
// collection held by an `owned` field, invoked after the receiver is gone.
// runAot: a closure stored in an Array is never released (#6212).
BEZEL_TEST(value_receiver_this_capture_by_copy_6721,
           record_this_closure_in_owned_array_field_accepted_spec_9_4) {
    BEZEL_EXPECT_EQ(stdoutPlain(
        "module M; "
        "public record Pt(X: int) "
        "  public method Adder(): (int) -> int "
        "    return lambda(d: int): int => this.X + d; "
        "  end Adder; "
        "end Pt; "
        "public class Holder "
        "  public owned Fs: Array<(int) -> int>; "
        "  public constructor() Fs := new Array<(int) -> int>(); end constructor; "
        "end Holder; "
        "public class Program "
        "  private static method Clobber(a: int, b: int, c: int, d: int): int "
        "    var p1 := a * 3 + b; "
        "    var p2 := c * 5 + d; "
        "    var p3 := p1 + p2; "
        "    return p3 + a + b + c + d; "
        "  end Clobber; "
        "  public static method Main() "
        "    var h := new Holder(); "
        "    h.Fs.Add(new Pt(7).Adder()); "
        "    Console.WriteLine(Clobber(111, 222, 333, 444)); "
        "    Console.WriteLine(h.Fs[0](5)); "
        "  end Main; "
        "end Program;"),
        "3774\n12\n");
}
