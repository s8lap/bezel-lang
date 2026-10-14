// Spec-derived regression tests for issue #6023 — the built-in exception half.
//
// THE RULE. Spec §5.1 "Reference conversions" (v0.338) converts a class only
// one way: "class to base class, transitively, up to `Object`". §31.4 fixes
// the built-in hierarchy (`IOException` derives from `Exception`,
// `FileNotFoundException` from `IOException`, `ArgumentException` from
// `Exception`, …), so `IOException` → `Exception` is implicit and
// `Exception` → `IOException` is a downcast, which §5.1 does not list.
//
// WHAT WAS BROKEN. The built-in exception types carry no TypeSymbol, so the
// declaration-keyed class-to-class check — the one that already says "a
// base-to-derived (downcast) conversion is not implicit" for user classes —
// never saw a built-in pair, and the pair fell to `checkAssignable`'s final
// catch-all accept. MEASURED on 6d8c82eb: the issue's `t01`,
//
//     public static method Handle(e: IOException): string => "io";
//     …  Console.WriteLine(Handle(new Exception("x")));
//
// compiled and printed `io`, and `var io: IOException? := e` with
// `e: Exception` compiled too. This is a DIFFERENT predicate arm from the
// `Object`-source one (tests/sema/object_downcast_not_implicit_6023_test.cpp);
// the two were fixed separately.
//
// Every row is on `runAot`. A reject row asserts: did not run, rejected with a
// real source location, and the diagnostic names both types and the rule.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

constexpr const char* kMainTail =
    " public static method Main(): int "
    "   Console.WriteLine(\"RAN\"); return 0; "
    " end Main; end P;";

void expectExceptionDowncastRejected(const JitResult& r, const char* base,
                                     const char* derived) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1): a "
           "base-to-derived (downcast) conversion is not implicit"));
    const std::string pair = std::string("`") + derived + "` derives from `" +
                             base + "`, not the reverse";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, pair.c_str()));
}

}  // namespace

// REJECT — THE ISSUE'S REPRO (`t01`): a fresh `Exception` passed where an
// `IOException` parameter is declared.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           issue_repro_exception_argument_to_ioexception_parameter_spec_5_1) {
    auto r = runAot(
        "module t01; "
        "public class Program "
        "  public static method Handle(e: IOException): string => \"io\"; "
        "  public static method Main() "
        "    Console.WriteLine(Handle(new Exception(\"x\"))); "
        "  end Main; "
        "end Program;");
    expectExceptionDowncastRejected(r, "Exception", "IOException");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — the issue's other shape: `Exception` into `IOException?`. §5.1
// "Nullable conversions" lifts only a conversion that exists.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_into_nullable_ioexception_local_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; public class P "
        "public static method M(e: Exception): int "
        "  var io: IOException? := e; return 0; end M;")
        + kMainTail);
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// REJECT — local initializer, non-nullable target.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_into_ioexception_local_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; public class P "
        "public static method M(e: Exception): string "
        "  var io: IOException := e; return io.Message; end M;")
        + kMainTail);
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// REJECT — assignment statement.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_assigned_to_ioexception_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; public class P "
        "public static method M(e: Exception, d: IOException): string "
        "  var io: IOException := d; io := e; return io.Message; end M;")
        + kMainTail);
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// REJECT — one level down the tree: `IOException` into its own subclass.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           ioexception_into_filenotfoundexception_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; public class P "
        "public static method M(e: IOException): string "
        "  var f: FileNotFoundException := e; return f.Message; end M;")
        + kMainTail);
    expectExceptionDowncastRejected(r, "IOException", "FileNotFoundException");
}

// REJECT — two levels: `Exception` into `ArgumentNullException`
// (`Exception` → `ArgumentException` → `ArgumentNullException`, §31.4).
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_into_grandchild_exception_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; public class P "
        "public static method M(e: Exception): string "
        "  var a: ArgumentNullException := e; return a.Message; end M;")
        + kMainTail);
    expectExceptionDowncastRejected(r, "Exception", "ArgumentNullException");
}

// REJECT — a transparent alias (§27.8) of the derived type is still the
// derived type.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_into_alias_of_ioexception_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; type IOE := IOException; public class P "
        "public static method M(e: Exception): string "
        "  var io: IOE := e; return io.Message; end M;")
        + kMainTail);
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// REJECT — SIBLINGS: neither is a base of the other, so this is not a
// downcast and gets the unrelated-types wording.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           ioexception_into_sibling_argumentexception_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module M6023E; public class P "
        "public static method M(e: IOException): string "
        "  var a: ArgumentException := e; return a.Message; end M;")
        + kMainTail);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the built-in exception type `IOException` is not a "
           "`ArgumentException`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "(downcast)"));
}

// ACCEPT and RUN — every WIDENING pair keeps working: built-in → its base and
// grand-base, through an alias, into a nullable base, a user exception class
// into its built-in bases, and a derived built-in at a base-typed parameter.
// Each line of stdout is produced THROUGH the widened reference.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_upcasts_still_accepted_and_run_spec_5_1) {
    auto r = runAot(
        "module M6023U; "
        "type IOE := IOException; "
        "public class MyEx: IOException "
        "  public constructor(m: string) base(m); end constructor; "
        "end MyEx; "
        "public class P "
        "  public static method Handle(e: IOException): string => e.Message; "
        "  public static method Up(e: Exception): string => e.Message; "
        "  public static method One(e: IOException) "
        "    var b: Exception := e; "
        "    Console.WriteLine(Up(e) + b.Message); "
        "    var a: IOE := e; "
        "    var c: Exception := a; "
        "    Console.WriteLine(c.Message); "
        "    var q: Exception? := e; "
        "    if q <> null then Console.WriteLine(\"q\"); end if; "
        "  end One; "
        "  public static method Two(m: MyEx) "
        "    Console.WriteLine(Handle(m)); "
        "    var i: IOException := m; "
        "    var x: Exception := m; "
        "    Console.WriteLine(x.Message + i.Message); "
        "  end Two; "
        "  public static method Three(f: FileNotFoundException, "
        "                             g: FileNotFoundException?) "
        "    Console.WriteLine(Handle(f) + Up(f)); "
        "    var h: IOException? := g; "
        "    if h = null then Console.WriteLine(\"h null\"); end if; "
        "  end Three; "
        "  public static method Main(): int "
        "    try throw new IOException(\"io\"); "
        "    catch var e: IOException => One(e), end try; "
        "    try throw new MyEx(\"mine\"); "
        "    catch var m: MyEx => Two(m), end try; "
        "    try throw new FileNotFoundException(\"fnf\"); "
        "    catch var f: FileNotFoundException => Three(f, null), end try; "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("ioio\nio\nq\nmine\nminemine\nfnffnf\nh null\n"));
}

// ACCEPT and RUN — the checked spellings (§12.7): `is` narrows an `Exception`
// carrier to the derived type, and `as` yields null for an exception that is
// not one.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           is_and_as_are_the_checked_exception_downcasts_spec_12_7) {
    auto r = runAot(
        "module M6023C; "
        "public class P "
        "  public static method Handle(e: IOException): string => e.Message; "
        "  public static method Probe(e: Exception) "
        "    if e is IOException then "
        "      var io: IOException := e; "
        "      Console.WriteLine(\"is \" + Handle(io)); "
        "    else "
        "      Console.WriteLine(\"not io\"); "
        "    end if; "
        "    var a := e as IOException; "
        "    if a = null then Console.WriteLine(\"as null\"); "
        "    else Console.WriteLine(\"as \" + a.Message); end if; "
        "  end Probe; "
        "  public static method Main(): int "
        "    try throw new IOException(\"disk\"); "
        "    catch var e: Exception => Probe(e), end try; "
        "    try throw new Exception(\"plain\"); "
        "    catch var e: Exception => Probe(e), end try; "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("is disk\nas disk\nnot io\nas null\n"));
}

// ACCEPT, PENDING #6083 — the two shapes whose narrowing the spec does not
// settle, with a built-in exception carrier: the path surviving an exiting
// `if e is not IOException then return …; end;`, and the body of a
// type-pattern arm `case e of IOException => …`. Both compiled and ran
// correctly before this fix, so `e` → `IOException` is kept accepted there
// until #6083 is ruled. Each method is entered with an `IOException` and with
// a plain `Exception`.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           pending_ruling_6083_exit_test_and_type_pattern_arm_keep_exception_downcast) {
    auto r = runAot(R"bzl(module M6023D;
public class P
  public static method Exit(e: Exception): string
    if e is not IOException then return "not io"; end if;
    var io: IOException := e;
    return io.Message;
  end Exit;
  public static method Arm(e: Exception): string
    case e of
      IOException => do var io: IOException := e; return io.Message; end,
      _ => do return "other"; end
    end case;
  end Arm;
  public static method Main(): int
    try throw new IOException("disk");
    catch var e: Exception => Console.WriteLine($"{Exit(e)} {Arm(e)}"), end try;
    try throw new Exception("plain");
    catch var e: Exception => Console.WriteLine($"{Exit(e)} {Arm(e)}"), end try;
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("disk disk\nnot io other\n"));
}

// REJECT — NOT pending. §7.4: "`not (x is T)` has the same truth value as
// `x is not T` but narrows neither branch", so the `not`-composed exit test
// keeps nothing.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           not_composed_exit_test_keeps_exception_downcast_rejected_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method Exit(e: Exception): string
    if not (e is IOException) then return "not io"; end if;
    var io: IOException := e;
    return io.Message;
  end Exit;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// ACCEPT + RUN (pending #6083, shape 1): an exception carrier, the exiting
// `is not` test with an `else` that falls through.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           pending_ruling_6083_exception_exit_test_with_non_exiting_else_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method T(e: Exception): string
    if e is not IOException then return "not io"; else Console.Write(""); end if;
    var io: IOException := e;
    return io.Message;
  end T;
  public static method Main(): int
    using var io := new IOException("disk");
    using var other := new Exception("other");
    Console.WriteLine($"{T(io)} {T(other)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("disk not io\n"));
}

// ACCEPT + RUN (pending #6083, shape 1): the positive spelling.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           pending_ruling_6083_exception_positive_test_with_exiting_else_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method T(e: Exception): string
    if e is IOException then Console.Write(""); else return "not io"; end if;
    var io: IOException := e;
    return io.Message;
  end T;
  public static method Main(): int
    using var io := new IOException("disk");
    using var other := new Exception("other");
    Console.WriteLine($"{T(io)} {T(other)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("disk not io\n"));
}

// ACCEPT + RUN, looped (#6120 with the built-in narrowing of this stack):
// `when c is IOException` narrows the catch binding in its arm's body (§7.4:
// "a `when` guard (§19.4, narrowing that arm's body)"), so `c` initialises an
// `IOException`.
// MEASURED on base: compiles, prints the same line.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           catch_arm_guard_narrows_builtin_exception_binding_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method Th(k: int)
    if k = 0 then throw new IOException("io"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is IOException => do var io: IOException := c; return io.Message.Length; end, var c2: Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..200 do t := t + T(0) + T(1); end for;
    Console.WriteLine($"{T(0)} {T(1)} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 -1 201\n"));
}

// ACCEPT + RUN: the same binding as a call argument.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           catch_arm_guard_narrowed_builtin_exception_binding_is_an_argument_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method Th(k: int)
    if k = 0 then throw new IOException("io"); end if;
    throw new Exception("exc");
  end Th;
  public static method Take(x: IOException): int => x.Message.Length;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is IOException => do return Take(c); end, var c2: Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)} {T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 -1\n"));
}

// REJECT: `or` proves neither operand.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           catch_arm_or_joined_guard_keeps_exception_downcast_rejected_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method Th(k: int)
    if k = 0 then throw new IOException("io"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is IOException or c is ArgumentException => do var io: IOException := c; return io.Message.Length; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// REJECT: §7.4 — never "by `not`-composition".
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           catch_arm_not_composed_guard_keeps_exception_downcast_rejected_spec_7_4) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method Th(k: int)
    if k = 0 then throw new IOException("io"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when not (c is not IOException) => do var io: IOException := c; return io.Message.Length; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// REJECT: a reassignment inside an `else`-less `if` ends the record.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           exception_exit_test_is_forgotten_when_reassigned_in_a_nested_if_spec_5_1) {
    auto r = runAot(R"bzl(module M6023E;
public class P
  public static method T(k: int, e: Exception, alt: Exception): string
    var q: Exception := e;
    if q is not IOException then return "not io"; end if;
    if k = 1 then q := alt; end if;
    var io: IOException := q;
    return io.Message;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    expectExceptionDowncastRejected(r, "Exception", "IOException");
}

// ACCEPT (#6023 review F2, the exception spelling). §12.7 (v0.338) narrows a
// `readonly` field by `is`; this compiler does not yet (#5550), so the read of
// `this._e` is left on the permissive path it had before #6023
// (isReadonlyFieldRead). Looped.
BEZEL_TEST(sema_builtin_exception_downcast_not_implicit_6023,
           readonly_exception_field_tested_then_read_as_the_tested_type_spec_12_7) {
    auto r = runAot(R"bzl(module M6023E;
public class Hr
  public constructor(e: Exception) _e := e; end constructor;
  public readonly _e: Exception;
  public method G(): int
    if this._e is IOException then var io: IOException := this._e; return io.Message.Length; end if;
    return -1;
  end G;
end Hr;
public class P
  public static method Main(): int
    var t := 0;
    for var i in 0..200 do t := t + new Hr(new IOException("abc")).G() + new Hr(new Exception("b")).G(); end for;
    Console.WriteLine($"{new Hr(new IOException("abc")).G()} {new Hr(new Exception("b")).G()} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3 -1 402\n"));
}
