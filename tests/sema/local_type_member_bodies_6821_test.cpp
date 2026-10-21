// Regression tests for #6821 (spec §21.10, v0.340): the member bodies of a
// local type are type-checked, and none of them may name a local or parameter
// of the bodies enclosing the declaration.
//
// THE GOVERNING RULE. §21.10 "Scope and name discipline" (v0.340): "The locals
// and parameters of the bodies enclosing the declaration, a primary-constructor
// parameter in scope there included, are not in scope anywhere inside it: not
// in its member or accessor bodies, field and property initializers, parameter
// defaults, contract clauses or enum value initializers. A local type has no
// closure ... A bare name there in a position §2.7 rule 1 resolves (a value
// read, an assignment target, a callee or a chain head) that the local type's
// own locals, parameters and members do not bind, and that is spelled like a
// local or parameter in scope at the declaration, is a compile error naming
// this rule ... a compile-time `const` local is no exception. ... Because those
// names are not in scope inside the local type, its own parameters and locals
// may reuse them."
//
// THE DEFECT. Two layers, both measured at staging 03f2d528 (and cd35e664):
//  1. ExprTC never visited a local type's member bodies at all. A `var q:
//     string := 5;` or an unknown name in a local record's method compiled;
//     codegen then degraded every read of a field or parameter in the body to
//     0 (an `internal:` warning only). The issue's repro printed `before`
//     with exit 0.
//  2. With the bodies checked, an enclosing local would resolve through the
//     method scope; the rule makes that a named compile error.
//
// ORACLE. runAot (the real AOT path). Reject rows assert the specific needle;
// accept rows assert the program's stdout, which at the base printed 0 for
// every one of them.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

constexpr const char* kNeedle =
    "names a local or parameter of the body that declares this local type";

bool printed(const bezel::test::JitResult& r, const char* want) {
    return r.ran && r.exitCode == 0 &&
           (r.stdoutText == std::string(want) ||
            r.stdoutText == [&] {
                std::string s;
                for (const char* p = want; *p; ++p) {
                    if (*p == '\n') s += '\r';
                    s += *p;
                }
                return s;
            }());
}

// ── Reject: the issue's literal repro ──
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_record_method_writes_enclosing_local) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var s := \"before\";\n"
        "    record Tmp(a: int)\n"
        "      public method Poke(): int\n"
        "        s := \"fromrecord\";\n"
        "        return a;\n"
        "      end Poke;\n"
        "    end Tmp;\n"
        "    var t := new Tmp(1);\n"
        "    _ := t.Poke();\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}

BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_object_local_write_from_record_method) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var sb := new StringBuilder();\n"
        "    var o: Object := sb;\n"
        "    record Tmp(a: int)\n"
        "      public method Poke(): int\n"
        "        o := \"fromrecord\";\n"
        "        return a;\n"
        "      end Poke;\n"
        "    end Tmp;\n"
        "    _ := new Tmp(1).Poke();\n"
        "    Console.WriteLine(o.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}

BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_record_method_reads_enclosing_parameter) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method F(k: int): int\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return a + k;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end F;\n"
        "  public static method Main(): int\n"
        "    return F(2);\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}

// "a compile-time `const` local is no exception"
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_const_local_is_no_exception) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    const K := 7;\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return a + K;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}

// "a callee or a chain head"
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_callee_and_chain_head) {
    const std::string callee =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var f := lambda(x: int) => x + 1;\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return f(a);\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(callee);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
    const std::string head =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var sb := new StringBuilder();\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        sb.Append(\"x\");\n"
        "        return a;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end Main;\n"
        "end C;\n";
    auto r2 = runAot(head);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r2));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r2, kNeedle));
}

// "even where §2.7 would go on to find ... a module declaration"
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_name_would_otherwise_find_a_module_declaration) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method Helper(): int\n"
        "    return 1;\n"
        "  end Helper;\n"
        "  public static method Main(): int\n"
        "    var Helper := 9;\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return Helper();\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}

// A local enum's method, a field initializer, a parameter default, a lambda
// inside the member, a nested type of the local type, and an iterator body.
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_every_body_position_is_covered) {
    const char* shapes[] = {
        // enum method
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    enum Col\n      Red, Green;\n      public method Bump(): int\n"
        "        return n;\n      end Bump;\n    end Col;\n    return 0;\n  end Main;\nend C;\n",
        // field initializer
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    record Tmp(a: int)\n      public Z: int := n;\n    end Tmp;\n"
        "    return 0;\n  end Main;\nend C;\n",
        // parameter default
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    record Tmp(a: int)\n      public method Get(x: int := n): int\n"
        "        return a + x;\n      end Get;\n    end Tmp;\n    return 0;\n  end Main;\nend C;\n",
        // lambda inside the member
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    record Tmp(a: int)\n      public method Get(): int\n"
        "        var f := lambda(x: int) => x + n;\n        return f(2);\n"
        "      end Get;\n    end Tmp;\n    return 0;\n  end Main;\nend C;\n",
        // nested type of the local type
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    record Outer(a: int)\n      public record In(b: int)\n"
        "        public method Get(): int\n          return b + n;\n"
        "        end Get;\n      end In;\n    end Outer;\n    return 0;\n  end Main;\nend C;\n",
        // iterator body (parameter and local)
        "module M;\nclass C\n  public static method Gen(k: int): IEnumerable<int>\n"
        "    record Tmp(a: int)\n      public method Get(): int\n        return a + k;\n"
        "      end Get;\n    end Tmp;\n    yield return new Tmp(1).Get();\n  end Gen;\n"
        "  public static method Main(): int\n    return 0;\n  end Main;\nend C;\n",
        // static member of the local type
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    record Tmp(a: int)\n      public static method Mk(): int\n        return n;\n"
        "      end Mk;\n    end Tmp;\n    return 0;\n  end Main;\nend C;\n",
        // property accessor
        "module M;\nclass C\n  public static method Main(): int\n    var n := 5;\n"
        "    record Tmp(a: int)\n      public property Dbl: int\n        get => a * 2 + n;\n"
        "      end Dbl;\n    end Tmp;\n    return 0;\n  end Main;\nend C;\n",
    };
    for (const char* src : shapes) {
        auto r = runAot(src);
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
    }
}

// ── The bodies are checked at all (layer 1) ──
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_body_type_errors_are_reported) {
    const std::string badInit =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    record Tmp(a: int)\n"
        "      public method Poke(): int\n"
        "        var q: string := 5;\n"
        "        return a;\n"
        "      end Poke;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Poke();\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(badInit);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not implicitly convertible"));
    const std::string unknown =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return a + zz;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end Main;\n"
        "end C;\n";
    auto r2 = runAot(unknown);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r2));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r2, "unresolved identifier `zz`"));
}

// ── Accept: what the rule leaves alone (over-rejection controls) ──

// "its own parameters and locals may reuse them"; the enclosing `n` is intact.
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_own_params_and_locals_reuse_enclosing_names) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var n := 5;\n"
        "    record Tmp(a: int)\n"
        "      public method Get(n: int): int\n"
        "        var s := n + a;\n"
        "        return s;\n"
        "      end Get;\n"
        "      public method Get2(): int\n"
        "        var n := 3;\n"
        "        return n + a;\n"
        "      end Get2;\n"
        "    end Tmp;\n"
        "    Console.WriteLine(new Tmp(1).Get(2));\n"
        "    Console.WriteLine(new Tmp(1).Get2());\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(printed(r, "3\n4\n5\n"));
}

// "that the local type's own ... members do not bind": a field spelled like an
// enclosing local is the field.
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_own_field_shadows_the_enclosing_name) {
    const std::string src =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := 100;\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return a;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    Console.WriteLine(new Tmp(1).Get());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(printed(r, "1\n"));
}

// "A type position, a named-argument label and a member name after `.` are not
// such lookups."
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_type_position_label_and_member_name_are_not_lookups) {
    const std::string src =
        "module M;\n"
        "record Pt(x: int);\n"
        "class C\n"
        "  public static method Add(a: int, n: int): int\n"
        "    return a + n;\n"
        "  end Add;\n"
        "  public static method Main(): int\n"
        "    var Pt := 1;\n"
        "    var n := 5;\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        var p: Pt := new Pt(a);\n"
        "        return C.Add(a := p.x, n := 2);\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    Console.WriteLine(new Tmp(5).Get());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(printed(r, "7\n"));
}

// Static members of the local type, an earlier local type, a sibling method's
// same-named local type, and a local enum's members all work in the body.
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_ordinary_member_bodies_run) {
    const std::string statics =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    record Tmp(a: int)\n"
        "      public static readonly K: int := 4;\n"
        "      public static method Make(): int\n"
        "        return K;\n"
        "      end Make;\n"
        "      public method Get(): int\n"
        "        return a + K + Make();\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    Console.WriteLine(new Tmp(1).Get());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(statics), "9\n"));
    const std::string chain =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    record A(x: int);\n"
        "    record B(y: int)\n"
        "      public method ToA(): A\n"
        "        return new A(y);\n"
        "      end ToA;\n"
        "    end B;\n"
        "    Console.WriteLine(new B(4).ToA().x);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(chain), "4\n"));
    const std::string siblings =
        "module M;\n"
        "class C\n"
        "  public static method F(): int\n"
        "    record Row(a: int)\n"
        "      public method Get(): int\n"
        "        return a + 1;\n"
        "      end Get;\n"
        "    end Row;\n"
        "    return new Row(1).Get();\n"
        "  end F;\n"
        "  public static method G(): int\n"
        "    record Row(b: int)\n"
        "      public method Get(): int\n"
        "        return b + 100;\n"
        "      end Get;\n"
        "    end Row;\n"
        "    return new Row(1).Get();\n"
        "  end G;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(F());\n"
        "    Console.WriteLine(G());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(siblings), "2\n101\n"));
    const std::string enumMembers =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    enum Col\n"
        "      Red, Green, Blue;\n"
        "      public static method First(): Col\n"
        "        return Col.Red;\n"
        "      end First;\n"
        "      public method Next(): Col\n"
        "        return Col.Blue;\n"
        "      end Next;\n"
        "    end Col;\n"
        "    Console.WriteLine(int(Col.First().Next()));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(enumMembers), "2\n"));
}

// A local type in a lambda, an async body and an iterator body.
BEZEL_TEST(local_type_member_bodies_6821, spec_21_10_local_type_in_lambda_async_and_iterator_bodies) {
    const std::string lam =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var f := lambda() => do\n"
        "      record Tmp(a: int)\n"
        "        public method Get(): int\n"
        "          return a + 10;\n"
        "        end Get;\n"
        "      end Tmp;\n"
        "      return new Tmp(1).Get();\n"
        "    end;\n"
        "    Console.WriteLine(f());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(lam), "11\n"));
    const std::string asy =
        "module M;\n"
        "class C\n"
        "  public static async method Run(): Task<int>\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return a + 10;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    return new Tmp(1).Get();\n"
        "  end Run;\n"
        "  public static async method Main(): Task<int>\n"
        "    var r := await Run();\n"
        "    Console.WriteLine(r);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(asy), "11\n"));
    const std::string it =
        "module M;\n"
        "class C\n"
        "  public static method Gen(): IEnumerable<int>\n"
        "    record Tmp(a: int)\n"
        "      public method Get(): int\n"
        "        return a + 10;\n"
        "      end Get;\n"
        "    end Tmp;\n"
        "    yield return new Tmp(1).Get();\n"
        "    yield return new Tmp(2).Get();\n"
        "  end Gen;\n"
        "  public static method Main(): int\n"
        "    for var x in Gen() do\n"
        "      Console.WriteLine(x);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(printed(runAot(it), "11\n12\n"));
}

}  // namespace
