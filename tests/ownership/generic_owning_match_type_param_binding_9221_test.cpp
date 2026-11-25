// fix #9221 (§35.5 *The owning match* / *The borrowing match*, §28.1 clause 3,
// §19.3 *Patterns*, §28.3 K3.4 / K8.4, v0.359): in a GENERIC body, a `case`
// arm binding whose type is a BARE TYPE PARAMETER `T` was not tracked as an
// owned (owning `case move`) or borrowed (borrowing `case`) local at all, so
// every ownership rule the non-generic twin's binding obeys was silently
// skipped.
//
// THE RULE. §28.1 clause 3: a generic body "type-checks under the class-or-
// interface assumption" — an `owned T` marker has its ordinary transfer
// meaning, and at a value or runtime-managed substitution it is "erased by the
// substitution … marker-keyed call-site and binding forms written under the
// class assumption remain admissible with no-op effect". §35.5: the transferred
// payload binding of an owning match "is an owned local of the arm body's
// scope, and §28.3's standard rules apply to it on every arm-exit edge"; a
// borrowing match's binding "is a borrow whose source is the scrutinee's
// source" (§19.3 verbatim). So a `T`-typed arm binding is classified exactly as
// the non-generic twin's class-typed binding is, and gets the same diagnostic.
//
// THE BUG (#9221). MEASURED on r30-c1 (eb536d988), every reject row below
// compiled clean for `T` unconstrained AND `where T: class`, while the same
// program with `Res` in place of `T` was rejected. Under
// `--sanitize=address`, the borrowing-case `move` and both `return v` shapes
// ran as a heap-use-after-free (the scrutinee's own §35.4 active-case disposal
// freed the payload a second time); the double move and the read after move
// ran to completion on a payload already handed to a sink. The binding was
// missing from the ownership pass's transfer set (`owningMatchTransferBindings_`)
// and its borrow set (`borrowOnlyArmBindings_`), and the escape and owned-sink
// checks keyed on `isBorrowableType`, which is false for a bare `T`.
//
// THE LEGAL ROWS. A nested generic that moves the payload into a fresh
// `Result<T, string>.Ok(move v)`, and the §35.5 `using var` adoption of the
// payload, were ALSO heap-use-after-frees on the base at the class
// substitution: the `move` did not null the binding's slot, so #9153's
// arm-exit auto-drop freed the moved payload. They are now clean.
//
// THE DISCARD ROWS. `_ := x` of a `T`-typed owned binding is an announced-
// consume discharge (§17.13). At a value or runtime-managed substitution the
// marker is erased (§28.1 clause 3), so it disposes nothing — but the codegen
// path dispatched the `int` / `string` value as an object (MEASURED on the
// base: stack-buffer-overflow for an `owned x: T` parameter at `T = int`).
// Once the arm binding is an owned local, the arm-binding spelling reached the
// same path; both spellings are pinned.
//
// OBSERVABILITY. A reject row's pre-fix behaviour is a CLEAN COMPILE, so the
// oracle is the SPECIFIC diagnostic the non-generic twin gets — never a bare
// `!r.ran` (runAot synthesizes an Error for a runtime crash too). Runtime rows
// are `runAotChecked` (AOT + ASan/LSan) and loop 200 times with an exact
// destructor count.
//
// per §35.5, §28.1 clause 3, §19.3, §28.3 K3.4/K8.4, §17.13, v0.359

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — the AOT oracles
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kDecls =
    "public class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "end Res; ";

// The non-generic twins' diagnostics (MEASURED: the `Res`-typed twin of each
// row is rejected with exactly this text on the base).
const char* kAlreadyMoved = "was already moved out at this point";
const char* kBorrowArmMove = "cannot `move` the `case` arm binding";
const char* kBorrowReturn = "a borrow cannot be returned as an `owned` value";
const char* kOwningArmValue =
    "this arm's value borrows owning-match payload binding `v`";

// `where` is "" (unconstrained T) or " where T: class".
std::string gen(const std::string& where, const std::string& methods) {
    return "public class G "
           "  public static method Sink<T>(owned x: T): int" + where +
           " return 0; end Sink; "
           "  public static method Peek<T>(x: T): int" + where +
           " return 1; end Peek; " + methods + "end G; ";
}

std::string prog(const std::string& extraDecls, const std::string& g,
                 const std::string& mainBody) {
    return std::string("module M; ") + kDecls + extraDecls + g +
           "public class Program public static method Main(): int " +
           mainBody + " end Main; end Program;";
}

const char* kLoopUse =
    "    var s := 0; var i := 0; "
    "    while i < 200 do "
    "      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res())); "
    "      s := s + G.Use<Res>(Result<Res, string>.Error(\"e\")); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "    return 0; ";

const char* kLoopUnwrap =
    "    var i := 0; "
    "    while i < 200 do "
    "      using var r1 := G.Unwrap<Res>(Result<Res, string>.Ok(new Res()), new Res()); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine($\"N={Res.N} Made={Res.Made}\"); "
    "    return 0; ";

std::string useMethod(const std::string& where, const std::string& caseStmt) {
    return "  public static method Use<T>(owned b: Result<T, string>): int" +
           where + " var k := 0; " + caseStmt + " return k; end Use; ";
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    return hasErrorContaining(r, needle);
}

// --- payload moved twice (§35.5 owned local + §28.3 K3.4) ------------------

const char* kTwice =
    "case move b of "
    "  Ok(var v) => do k := G.Sink<T>(move v); k := G.Sink<T>(move v); end, "
    "  Error(var e) => k := 2 "
    "end case;";

}  // namespace

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_payload_moved_twice_unconstrained_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen("", useMethod("", kTwice)), kLoopUse), kAlreadyMoved));
}

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_payload_moved_twice_where_class_rejected) {
    const std::string w = " where T: class";
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen(w, useMethod(w, kTwice)), kLoopUse), kAlreadyMoved));
}

// The same rule in the ERROR position and on a user-declared generic choice:
// the classification reads the binding's type, not the payload slot's index.
BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_error_position_payload_moved_twice_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("",
             gen("",
                 "  public static method Use<T>(owned b: Result<string, T>): int "
                 "    var k := 0; "
                 "    case move b of "
                 "      Ok(var v) => k := 1, "
                 "      Error(var e) => do k := G.Sink<T>(move e); k := G.Sink<T>(move e); end "
                 "    end case; "
                 "    return k; "
                 "  end Use; "),
             "    var s := G.Use<Res>(Result<string, Res>.Error(new Res())); "
             "    return s; "),
        "owned value in `e` was already moved out at this point"));
}

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_user_choice_payload_moved_twice_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public choice MyOpt<T> Some(owned v: T), None; end MyOpt; ",
             gen("",
                 "  public static method Use<T>(owned b: MyOpt<T>): int "
                 "    var k := 0; "
                 "    case move b of "
                 "      Some(var v) => do k := G.Sink<T>(move v); k := G.Sink<T>(move v); end, "
                 "      None => k := 2 "
                 "    end case; "
                 "    return k; "
                 "  end Use; "),
             "    var s := G.Use<Res>(MyOpt<Res>.Some(new Res())); "
             "    return s; "),
        kAlreadyMoved));
}

// --- read after move (§28.3 K3.4) -------------------------------------------

namespace {
const char* kReadAfterMove =
    "case move b of "
    "  Ok(var v) => do k := G.Sink<T>(move v); k := G.Peek<T>(v); end, "
    "  Error(var e) => k := 2 "
    "end case;";
}  // namespace

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_28_3_read_after_move_unconstrained_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen("", useMethod("", kReadAfterMove)), kLoopUse),
        kAlreadyMoved));
}

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_28_3_read_after_move_where_class_rejected) {
    const std::string w = " where T: class";
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen(w, useMethod(w, kReadAfterMove)), kLoopUse),
        kAlreadyMoved));
}

// --- `move` out of a BORROWING case (§35.5 *The borrowing match*, §19.3) ---
// MEASURED on the base: accepted, ASan heap-use-after-free.

namespace {
const char* kBorrowMove =
    "case b of Ok(var v) => k := G.Sink<T>(move v), Error(var e) => k := 2 "
    "end case;";
}  // namespace

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_borrowing_case_move_unconstrained_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen("", useMethod("", kBorrowMove)), kLoopUse),
        kBorrowArmMove));
}

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_borrowing_case_move_where_class_rejected) {
    const std::string w = " where T: class";
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen(w, useMethod(w, kBorrowMove)), kLoopUse),
        kBorrowArmMove));
}

// A generic RECORD destructure binds a borrow too (§19.3).
BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_19_3_generic_record_destructure_move_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public record Box<T>(V: T); ",
             gen("",
                 "  public static method Use<T>(r: Box<T>): int "
                 "    var k := 0; "
                 "    case r of Box(var a) => k := G.Sink<T>(move a) end case; "
                 "    return k; "
                 "  end Use; "),
             "    using var rr := new Res(); "
             "    var s := G.Use<Res>(new Box<Res>(rr)); "
             "    return s; "),
        kBorrowArmMove));
}

// --- `return v` of a BORROWING case's binding (§28.8) ----------------------
// MEASURED on the base: accepted, ASan heap-use-after-free.

namespace {
std::string unwrapMethod(const std::string& where) {
    return std::string(
               "  public static method Unwrap<T>(owned b: Result<T, string>, "
               "owned d: T): owned T") +
           where + " case b of "
           "  Ok(var v) => do return v; end, "
           "  Error(var e) => do return d; end "
           "end case; end Unwrap; ";
}
}  // namespace

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_28_8_borrowing_case_return_binding_unconstrained_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen("", unwrapMethod("")), kLoopUnwrap), kBorrowReturn));
}

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_28_8_borrowing_case_return_binding_where_class_rejected) {
    const std::string w = " where T: class";
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", gen(w, unwrapMethod(w)), kLoopUnwrap), kBorrowReturn));
}

// --- the payload escaping an OWNING expression arm (§35.5, K8.4) ---------
// An unmarked `=> v` arm result is a BORROW arm (§28.3 arm-meet row); the
// marked arm transfer `=> move v` is K3.4's admitted spelling. The non-
// generic twin is rejected with the payload-escape diagnostic; MEASURED on the
// base the generic form was accepted.
//
// (The STATEMENT-position `return v` out of a `case move` arm is rejected with
// the twin's K8.4 diagnostic too, but it is deliberately NOT pinned here: the
// twin's own rejection is in tension with §35.5's "owned local" reading and
// K3.2's marker-free `return` — #9228.)

BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_owning_case_expression_arm_value_escape_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("",
             gen("",
                 "  public static method Use<T>(owned b: Result<T, string>, d: T): int "
                 "    var got := (case move b of Ok(var v) => v, Error(var e) => d); "
                 "    return G.Peek<T>(got); "
                 "  end Use; "),
             "    using var dd := new Res(); "
             "    var s := G.Use<Res>(Result<Res, string>.Ok(new Res()), dd); "
             "    return s; "),
        kOwningArmValue));
}

// --- legal shapes stay accepted and runtime-clean ---------------------------

// A nested generic moving the payload into a fresh owning choice (§35.5: a
// single transfer out of the arm). MEASURED on the base: heap-use-after-free
// in `G$Use$spec$Res` (the moved slot was not nulled, so the arm-exit
// auto-drop freed the payload the callee had already disposed).
BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_nested_generic_single_move_runs_clean) {
    auto r = runAotChecked(prog(
        "",
        gen("",
            "  public static method Use2<T>(owned b: Result<T, string>): int "
            "    var k := 0; "
            "    case move b of Ok(var v) => k := G.Sink<T>(move v), Error(var e) => k := 9 end case; "
            "    return k; "
            "  end Use2; "
            "  public static method Use<T>(owned b: Result<T, string>): int "
            "    var k := 0; "
            "    case move b of "
            "      Ok(var v) => k := G.Use2<T>(Result<T, string>.Ok(move v)), "
            "      Error(var e) => k := 2 "
            "    end case; "
            "    return k; "
            "  end Use; "),
        "    var s := 0; var i := 0; "
        "    while i < 200 do "
        "      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res())); "
        "      s := s + G.Use<Res>(Result<Res, string>.Error(\"e\")); "
        "      i := i + 1; "
        "    end while; "
        "    var q := G.Use<int>(Result<int, string>.Ok(5)) + G.Use<int>(Result<int, string>.Error(\"e\")); "
        "    q := q + G.Use<string>(Result<string, string>.Ok(\"x\")); "
        "    Console.WriteLine($\"q={q} N={Res.N} Made={Res.Made} S={s}\"); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "q=2 N=200 Made=200 S=400\n");
}

// §35.5's `using var` adoption of the transferred payload, `where T: class`.
// MEASURED on the base: heap-use-after-free (same cause as above).
BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_35_5_using_var_adoption_of_payload_runs_clean) {
    const std::string w = " where T: class";
    auto r = runAotChecked(prog(
        "",
        gen(w, useMethod(w,
                         "case move b of "
                         "  Ok(var v) => do using var u := move v; k := G.Peek<T>(u); end, "
                         "  Error(var e) => k := 2 "
                         "end case;")),
        kLoopUse));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "N=200 Made=200 S=600\n");
}

// `_ := x` of an `owned x: T` parameter at a VALUE substitution is a no-op
// (§28.1 clause 3, §17.13). MEASURED on the base: stack-buffer-overflow at
// `T = int` (the `int` was dispatched as an object).
BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           spec_28_1_discard_of_owned_type_param_at_value_substitution_runs_clean) {
    auto r = runAotChecked(prog(
        "",
        gen("",
            "  public static method Use<T>(owned x: T): int "
            "    _ := x; "
            "    return 1; "
            "  end Use; "),
        "    var q := 0; var i := 0; "
        "    while i < 200 do "
        "      q := q + G.Use<int>(5) + G.Use<string>(\"x\"); "
        "      q := q + G.Use<Res>(new Res()); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"q={q} N={Res.N} Made={Res.Made}\"); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "q=600 N=200 Made=200\n");
}

// CONTROL (green on the base; the boundary this fix must not cross): the
// same discard of an OWNING-ARM binding of type `T`, at the class, `int` and
// `string` substitutions. The binding is now an owned local, so it reaches the
// announced-consume path; it must still dispose exactly once at the class
// substitution and nothing at the value ones.
BEZEL_TEST(generic_owning_match_type_param_binding_9221,
           control_spec_17_13_discard_of_owning_arm_binding_runs_clean) {
    auto r = runAotChecked(prog(
        "",
        gen("", useMethod("",
                          "case move b of "
                          "  Ok(var v) => do _ := v; k := 1; end, "
                          "  Error(var e) => k := 2 "
                          "end case;")),
        "    var s := 0; var i := 0; "
        "    while i < 200 do "
        "      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res())); "
        "      s := s + G.Use<Res>(Result<Res, string>.Error(\"e\")); "
        "      s := s + G.Use<int>(Result<int, string>.Ok(5)); "
        "      s := s + G.Use<string>(Result<string, string>.Ok(\"x\")); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "N=200 Made=200 S=1000\n");
}
