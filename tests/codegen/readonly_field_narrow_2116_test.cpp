// Codegen (AOT) — §7.4 readonly-field flow narrowing, deterministic redo
// (v0.214, proposal 0058; #2085 reintroduced per #2116).
//
// Runtime-value companions to the new #2116 design cells (the original #2085
// cells live in readonly_field_narrow_2085_test.cpp, restored verbatim):
// the lambda boundary (#2113 closed for the field-fact set), the corrected
// elif-condition state, the loop-entry fact survival (#2118 analogue), and
// await survival. These programs must COMPILE (sema accepts the narrowed
// access) AND produce the correct value at runtime, observable via the int
// exit code. The reject sides live in the lit/FileCheck track
// (tests/lit/sema/readonly_field_narrowing_2116__*).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §7.4 (v0.214) / #2116: an ambient (this-rooted) fact established in the
// enclosing region is valid inside a lambda body created there — the readonly
// field holds its observed non-null value whenever the closure runs.
// "hello".Length == 5 through the invoked closure.
BEZEL_TEST(codegen_readonly_field_narrow_2116, lambda_sees_enclosing_fact) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Use(): int "
        "    if _name <> null then "
        "      var f := lambda() => _name.Length; return f(); "
        "    end; return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var c := new C(\"hello\"); return c.Use(); "
        "  end Main; end C;"), 5);
}

// §7.4 (v0.214) / #2116: an elif CONDITION sees the false-branch facts of the
// preceding conditions — `_name = null` was false, so the elif's
// `_name.Length > 0` and its body's read are sound. "hey".Length == 3.
BEZEL_TEST(codegen_readonly_field_narrow_2116, elif_else_fact_reads_payload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _name = null then return 7; "
        "    elif _name.Length > 0 then return _name.Length; end; return 9; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(\"hey\"); return c.Len(); "
        "  end Main; end C;"), 3);
}

// §7.4 (v0.214) / #2116: the null referent takes the `then` path — the elif
// condition (whose compile-time admission relied on the else-fact) never runs.
BEZEL_TEST(codegen_readonly_field_narrow_2116, elif_else_null_takes_then) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _name = null then return 7; "
        "    elif _name.Length > 0 then return _name.Length; end; return 9; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(null); return c.Len(); "
        "  end Main; end C;"), 7);
}

// §7.4 (v0.214) / #2118 / #2116: a loop-ENTRY ambient fact survives the while
// body (with an intervening call) and the after-loop join; both the in-body
// and after-loop reads load the payload. "world".Length == 5 (i counts to 10,
// exit code is the final Length read).
BEZEL_TEST(codegen_readonly_field_narrow_2116, while_entry_fact_survives) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Noise() end Noise; "
        "  public method Len(): int "
        "    if _name <> null then "
        "      var i := 0; "
        "      while i < 10 do Noise(); i := i + _name.Length; end; "
        "      return _name.Length; "
        "    end; return 0; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(\"world\"); return c.Len(); "
        "  end Main; end C;"), 5);
}

// §7.4 (v0.214) / #2116: a CLASS-typed nullable readonly field narrows and
// METHOD DISPATCH through the narrowed path reads the correct receiver (a
// nullable class reference is a bare pointer — no tagged-optional extraction
// is involved, unlike the value-type receiver dispatch tracked as #2106).
// Logger.Tag() + 1 == 22.
BEZEL_TEST(codegen_readonly_field_narrow_2116, class_field_method_dispatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Logger "
        "  public method Tag(): int return 21; end Tag; "
        "  public constructor() end constructor; "
        "  destructor end destructor; end Logger; "
        "class C public readonly _log: Logger?; "
        "  public constructor(l: Logger?) _log := l; end constructor; "
        "  destructor end destructor; "
        "  public method Use(): int "
        "    if _log <> null then return _log.Tag() + 1; end; return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var l := new Logger(); using var c := new C(l); "
        "    return c.Use(); "
        "  end Main; end C;"), 22);
}

// §7.4 (v0.214) / #2116: an ambient fact survives an `await` — the readonly
// field has no writer for an interleaving continuation to run (§30.13/§7.4).
// "hey".Length + Step()'s 1 == 4.
BEZEL_TEST(codegen_readonly_field_narrow_2116, fact_survives_await) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public async method Step(): Task<int> return 1; end Step; "
        "  public async method Len(): Task<int> "
        "    if _name <> null then "
        "      var k := await Step(); return _name.Length + k; "
        "    end; return 0; "
        "  end Len; "
        "  public static async method Main(): Task<int> "
        "    using var c := new C(\"hey\"); return await c.Len(); "
        "  end Main; end C;"), 4);
}
