// fix #5319 — a computed property that reads ITSELF on another instance
// crashed bezelc (SIGSEGV from unbounded getter inlining) — spec §23.2, v0.313.
//
// ── ROOT CAUSE (traced through the codegen call graph, not guessed) ───────
// §23.2 admits an expression-bodied read-only property, `property P: T =>
// expr`, and imposes NO restriction on which expressions `expr` may read. In
// particular nothing forbids `expr` from reading `P` on another instance, so
//     property Rec: int => (if _child = null then _f else _child!.Rec);
// is a legal, run-time-TERMINATING program: each hop moves one link down a
// finite chain and the last link returns `_f`.
//
// Codegen, however, INLINES an arrow-bodied getter at each read site: the
// read site sets `currentThis_` to the receiver and calls
// `lowerExpr(*getter)` (LlvmBackend_Exprs.cpp). Nothing recorded that the
// getter body being inlined is the very body we are already inside, so the
// `..._child!.Rec` read inside the getter re-entered the INLINER, which
// inlined the getter again, and so on. The compiler thread's stack grew until
// it hit the guard page — the issue's exit 139 (SIGSEGV, core dumped).
//
// The block-form twin of the same read is NOT affected: its body is emitted
// ONCE into a standalone `bz$<C>$<P>$propget` function
// (`ensurePropertyBlockGetter`) and the read site CALLS it, so recursion
// terminates at the call boundary. Only the arrow form was unbounded.
//
// ── THE FIX ──────────────────────────────────────────────────────────────
// A guard on the CHAIN OF PROPERTY SYMBOLS being inlined
// (`computedGetterInlining_` / `ComputedGetterInlineScope`, LlvmBackend_Exprs
// .cpp + LlvmBackend_impl.hpp), applied at every one of the five arrow-getter
// inlining sites. When the property about to be inlined is already on the
// chain, the read site CALLS the same materialised accessor
// `ensurePropertyAccessorFn` that the vtable/itable and exported faces already
// call for this property (`callComputedGetterFn`,
// LlvmBackend_Assign.cpp) instead of inlining the body again. The call is
// ordinary function-call recursion, so it terminates exactly when the
// program's own recursion terminates.
//
// WHY A SYMBOL CHAIN, NOT A DEPTH COUNTER. A depth bound would silently
// change the lowering of every DEEP-but-acyclic getter chain (a legal 200-deep
// `A.b.c.d…` would stop inlining and start calling), which is a behaviour
// change far wider than this bug. Keying on the chain of symbols fires ONLY on
// a genuine self- or mutual read, so for every non-recursive program the guard
// is a no-op and the emitted code is what it was pre-fix. T5 and T6 pin that
// for a 6-link and a 41-link acyclic chain respectively.
//
// WHY REUSE `ensurePropertyAccessorFn` RATHER THAN A NEW EMITTER. It already
// materialises `T get(ptr this)` for an arrow-bodied property, memoised on
// (property, isSet, specialization tag), with the full #2702/#2694 nested-body
// state isolation, and it is what the vtable/itable and exported faces already
// call for this same property. Reusing it means a property that is BOTH
// recursive and virtual keeps ONE accessor body — the recursive read compiles
// to a call of the very function its own vtable slot names.
//
// ── MEASURED (probes run through `scripts/agent_check.sh --probe --asan`, at
// this branch's base and with this change; the gtest rows re-measure the same
// shapes on the `runAot` oracle) ───────────────────────────────────────────
// Pre-fix bezelc exits 139 (SIGSEGV, core dumped) on T1's qualified `obj.P`
// read, on T2's `obj?.P` safe-navigation read, on T3's mutual A→B→A cycle, and
// on T4's static `S`-reads-`S` (measured for each individually, not inferred
// from one repro). Post-fix all four COMPILE and RUN, each printing the
// arithmetically correct value.
//
// The FIFTH guarded site — the record-devirtualised read in
// `lowerVirtualPropertyGet` — is guarded for uniformity with its four siblings
// but was NOT separately observed to crash pre-fix: reaching it needs a record
// whose implementing property's arrow body reads the property back through
// the interface it implements, and no such repro was found. Its guard is the
// same three lines as the other four, so it is carried by their evidence
// rather than by a row of its own; this is stated rather than papered over.
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────
//   T1  THE ISSUE'S OWN REPRO (§23.2): `Rec` reads `_child!.Rec`. Pre-fix
//       SIGSEGV. Post-fix returns the chain-WALK's leaf value (200), not a
//       sum — the guard is about the compiler, the arithmetic is unchanged.
//   T2  safe-navigation spelling `obj?.P` — a SEPARATE inlining site,
//       independently measured to crash pre-fix.
//   T3  MUTUAL recursion: A's getter reads B's, B's reads A's. The chain guard
//       must catch a cycle that does not contain a self-read.
//   T4  STATIC property self-read — a third, separately-crashing site
//       (the accessor takes no receiver, §22.12 / #2885).
//   T5  DEEP-BUT-ACYCLIC control: a 6-link getter chain, each link reading the
//       NEXT (never itself) is legal per §23.2 and MUST still inline. This is
//       the row that pins the "symbol chain, not a depth counter" decision —
//       a depth bound would still pass it, so it is joined by T6.
//   T6  LONG-ACYCLIC control: a 41-link chain, well past the depth at which a
//       plausible depth-counter implementation would have started bailing
//       out. Still inlines and returns 41.
//   T7  ACCEPT-SIDE CONTROL: a NON-recursive computed property reads a
//       DIFFERENT property — pins that the common (non-recursive) inline path
//       is untouched and still yields the right answer.
//   T8  ACCEPT-SIDE CONTROL: the BLOCK-bodied spelling of T1's shape, which
//       already worked pre-fix (it calls `ensurePropertyBlockGetter`), must
//       keep producing the same answer as the arrow form.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

namespace {

// T9–T11's shared runner. The 200-iteration loop is the ONE-SHOT-ORACLE net
// this family needs: the defect these rows pin is not "the answer is wrong
// once", it is "the accessor compiled against `Box<int>`'s layout is re-entered
// on a `Box<long>` receiver" — an ABI mismatch whose damage depends on what the
// mis-laid-out load produces on each iteration, not on a single read. The
// program counts its own wrong answers and PRINTS the total (an 8-bit exit code
// truncates at 256 iterations, so the count belongs on stdout), and the row
// asserts the printed total is `0`.
std::string runLoop200(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

}  // namespace

// ── T1 — the issue's own repro (§23.2, v0.313). The getter reads the SAME
// property on a DIFFERENT instance, which the spec admits and which
// terminates at run time. Pre-fix: bezelc exits 139 (SIGSEGV).
BEZEL_TEST(codegen_issue_5319,
           computed_property_reading_itself_on_another_instance_compiles_and_runs_spec_23_2) {
    auto r = runAot(
        "module M5319A; "
        "class Rec5319A "
        "  private _f: int; "
        "  private owned _child: Rec5319A?; "
        "  public constructor(f: int, owned c: Rec5319A?) _f := f; _child := c; end constructor; "
        // §23.2: nothing forbids the body reading `Rec` again — on another
        // instance. Each hop consumes one link and the last link returns
        // `_f`, so this terminates (it is a chain-walk, not a sum).
        "  public property Rec: int => (if _child = null then _f else _child!.Rec); "
        "end Rec5319A; "
        "class C "
        "  public static method Main(): int "
        "    using var r := new Rec5319A(7, new Rec5319A(30, new Rec5319A(200, null))); "
        "    return r.Rec; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // MEASURED post-fix: the chain-walk returns the INNERMOST leaf's `_f`,
    // i.e. 200 — not a sum. 200 < 256 so the 8-bit exit code carries it.
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// ── T2 — safe-navigation spelling. A SEPARATE inlining site from T1's
// (LlvmBackend_Exprs.cpp, the `?.` non-null access block), independently
// measured to crash pre-fix.
BEZEL_TEST(codegen_issue_5319,
           computed_property_safe_navigation_self_read_compiles_and_runs_spec_23_2_6_4) {
    auto r = runAot(
        "module M5319B; "
        "class Rec5319B "
        "  private _f: int; "
        "  private owned _child: Rec5319B?; "
        "  public constructor(f: int, owned c: Rec5319B?) _f := f; _child := c; end constructor; "
        // The `?.` spelling of the self-read. The `?? _f` is required by §7:
        // `?.` yields `int?` and the property's declared type is `int`.
        "  public property Rec: int => (if _child = null then _f else (_child?.Rec ?? _f)); "
        "end Rec5319B; "
        "class C "
        "  public static method Main(): int "
        "    using var r := new Rec5319B(5, new Rec5319B(40, null)); "
        "    return r.Rec; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // MEASURED post-fix: 40 — the leaf's `_f`.
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ── T3 — MUTUAL recursion. A's getter reads B's and B's reads A's, so NO
// body contains a self-read; the chain guard must still catch the cycle. This
// is the row a naive "is this the same expression" check would miss.
BEZEL_TEST(codegen_issue_5319,
           mutually_recursive_computed_properties_compile_and_run_spec_23_2) {
    auto r = runAot(
        "module M5319C; "
        "class Ping5319C "
        "  private _n: int; "
        "  private owned _pong: Pong5319C?; "
        "  public constructor(n: int, owned p: Pong5319C?) _n := n; _pong := p; end constructor; "
        "  public property V: int => (if _pong = null then _n else _pong!.V); "
        "end Ping5319C; "
        "class Pong5319C "
        "  private _n: int; "
        "  private owned _ping: Ping5319C?; "
        "  public constructor(n: int, owned p: Ping5319C?) _n := n; _ping := p; end constructor; "
        "  public property V: int => (if _ping = null then _n else _ping!.V); "
        "end Pong5319C; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Ping5319C(9, new Pong5319C(90, null)); "
        "    return p.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // MEASURED post-fix: 90 — Pong's `_n`; the leaf the cycle finally lands on.
    BEZEL_EXPECT_EQ(r.exitCode, 90);
}

// ── T4 — STATIC property self-read. A third, separately-crashing site: the
// materialised accessor takes NO receiver (§22.12, fix #2885), so the guarded
// read site must call it with an empty argument list, not `this`. The
// self-read is made TERMINATING by having the guard `Tick()` advance the
// counter, so the recursive hops are finite and the answer is checkable.
BEZEL_TEST(codegen_issue_5319,
           static_computed_property_self_read_compiles_and_runs_spec_23_2_22_12) {
    auto r = runAot(
        "module M5319D; "
        "class C "
        "  private static _d: int; "
        // Advances the counter as its side effect, so the guard is false on
        // the fourth evaluation and the recursion bottoms out.
        "  private static method Tick(): bool\n"
        "    _d := _d - 1;\n"
        "    return _d > 0;\n"
        "  end Tick;\n"
        // Reads ITSELF, on the same (static) instance.
        "  public static property S: int => (if Tick() then S + 10 else 77);\n"
        "  public static method Main(): int "
        "    _d := 3; "
        "    return S; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // MEASURED post-fix: 97 = 77 + 2×10 — `Tick()` runs 3 times (d 3→2→1→0)
    // and the guard is true on the first TWO, so exactly two `+10` hops
    // happen above the 77 base.
    BEZEL_EXPECT_EQ(r.exitCode, 97);
}

// ── T5 — DEEP-BUT-ACYCLIC control (§23.2). A chain of computed properties
// where each link reads the NEXT and never itself is legal and must still be
// INLINED — this is the common case the guard must not perturb.
BEZEL_TEST(codegen_issue_5319,
           deep_acyclic_computed_property_chain_still_inlines_spec_23_2_accept_control) {
    auto r = runAot(
        "module M5319E; "
        "class Ch5319E "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property P1: int => _n + 1; "
        "  public property P2: int => P1 + 10; "
        "  public property P3: int => P2 + 100; "
        "  public property P4: int => P3 + 1000; "
        "  public property P5: int => P4 + 10000; "
        "  public property P6: int => P5 + 3; "
        "end Ch5319E; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Ch5319E(1); "
        "    return c.P6 - 11114; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // 1 + 1 + 10 + 100 + 1000 + 10000 + 3 = 11115, minus 11114 = 1.
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── T6 — LONG-ACYCLIC control. The chain is 41 links deep, far past the depth
// at which a plausible depth-counter implementation would have stopped
// inlining. A depth bound would silently change this program's lowering; the
// symbol-chain guard must leave it entirely alone. Still inlines: 41 nested
// `+1` frames composed in one expression.
BEZEL_TEST(codegen_issue_5319,
           long_acyclic_computed_property_chain_still_inlines_spec_23_2_accept_control) {
    std::string src =
        "module M5319F; "
        "class Ch5319F "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property P1: int => _n + 1; ";
    for (int i = 2; i <= 41; ++i)
        src += "  public property P" + std::to_string(i) + ": int => P" +
               std::to_string(i - 1) + " + 1; ";
    src +=
        "end Ch5319F; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Ch5319F(0); "
        "    return c.P41; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 links × +1 = 41.
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ── T7 — ACCEPT-SIDE CONTROL: a NON-recursive computed property reading a
// DIFFERENT property. This is the overwhelmingly common shape; pins that the
// ordinary inline path is untouched by the guard and still computes correctly.
BEZEL_TEST(codegen_issue_5319,
           non_recursive_computed_property_reading_another_property_unaffected_spec_23_2_accept_control) {
    auto r = runAot(
        "module M5319G; "
        "class Pair5319G "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor(a: int, b: int) _a := a; _b := b; end constructor; "
        "  public property A: int => _a; "
        "  public property B: int => _b; "
        "  public property Sum: int => A + B; "
        "  public property Quad: int => Sum * 4; "
        "end Pair5319G; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair5319G(11, 31); "
        "    return p.Quad; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 168);
}

// ── T8 — ACCEPT-SIDE CONTROL: the BLOCK-bodied spelling of T1's shape. It
// already worked pre-fix (the read site CALLS `ensurePropertyBlockGetter`
// rather than inlining) and must keep producing the identical answer — this
// is the precedent the fix makes the arrow form match.
BEZEL_TEST(codegen_issue_5319,
           block_bodied_computed_property_self_read_unaffected_spec_23_2_23_3_accept_control) {
    auto r = runAot(
        "module M5319H; "
        "class Rec5319H "
        "  private _f: int; "
        "  private owned _child: Rec5319H?; "
        "  public constructor(f: int, owned c: Rec5319H?) _f := f; _child := c; end constructor; "
        "  public property Rec: int "
        "    get => do\n"
        "      if _child = null then\n"
        "        return _f;\n"
        "      end if;\n"
        "      return _child!.Rec;\n"
        "    end;\n"
        "  end Rec;\n"
        "end Rec5319H; "
        "class C "
        "  public static method Main(): int "
        "    using var r := new Rec5319H(3, new Rec5319H(39, null)); "
        "    return r.Rec; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    // MEASURED post-fix: 39 — identical to T1's arrow-form leaf, which is the
    // point: the arrow and block spellings agree.
    BEZEL_EXPECT_EQ(r.exitCode, 39);
}

// ════════════════════════════════════════════════════════════════════════════
// T9–T11 — the GENERIC-OWNER fix-forward (lane 5319-ff).
//
// WHAT T1–T8 DID NOT COVER, AND WHY IT MATTERED. Every row above declares a
// NON-generic class, so the accessor T1–T4 materialise is the only one that
// class shape can ever need. §14.1 admits `class Box<T>` and a computed
// property on it, and §23.2 puts no restriction on the body's expressions, so
//
//     class Box<T>
//       private _v: T;
//       private _next: Box<T>?;
//       public property Rec: T => (if _next = null then _v else _next!.Rec);
//     end Box<T>;
//
// is the SAME legal, run-time-terminating program as T1's, now with the type
// parameter in every position. With TWO live instantiations (`Box<int>` and
// `Box<long>`) the two have DIFFERENT layouts — `T` is 4 bytes in one and 8 in
// the other, so `_v` and `_next` sit at different offsets — and the T1–T8 rows,
// being single-instantiation, could never see it.
//
// THE DEFECT THESE ROWS PIN. `callComputedGetterFn` reached
// `ensurePropertyAccessorFn(cls, ps, false)` DIRECTLY. That builder memoises on
// (property, isSet, `accessorSpecTagNow()`), and at an ordinary read site no
// specialization tag is installed — `accessorSpecTag_` is 0, the "shared/erased
// template" key. So the FIRST instantiation read built the accessor body
// against the ERASED `T -> i32` layout, memoised it under tag 0, and every
// later read — including the second instantiation's — got that same body back.
// The `Box<int>`-compiled accessor then recursed on a `Box<long>` receiver and
// read `_next` at the int offset, so the walk never found the null base and ran
// off the end of the object. MEASURED at this branch's pre-fix-forward state:
// bezelc exits 139 (SIGSEGV) on T9, and T10/T11 abort in LLVM IR verification
// ("PHI node operands are not the same type") — the tag-0 body, compiled for
// one `T`, having its own recursive call wired against another's.
//
// The vtable and itable faces never had this bug because they do NOT reach
// `ensurePropertyAccessorFn` directly: they install the per-(class,
// substitution) tag `accessorSpecTagFor` mints, snapshot the substitution under
// it, and only then call the builder (`buildSpecializedAccessorFn`,
// LlvmBackend_Setup.cpp). T9–T11 are the fix-forward making the recursive-read
// path use that same vehicle, so a generic owner's recursive read names the
// accessor for ITS OWN instantiation.
//
// The 200-iteration loop (see `runLoop200`) is the one-shot net: an
// off-by-one-offset `_next` load is not reliably wrong on iteration 1 — it is
// wrong according to what the adjacent bytes happen to hold, which is why a
// single read is a weak oracle for exactly this class of bug.
// ════════════════════════════════════════════════════════════════════════════

// ── T9 — `Box<int>` AND `Box<long>`, both live. The two instantiations differ
// in WIDTH (`T` is i32 vs i64), so the erased-template reuse lands `_next` at
// the wrong offset. Pre-fix-forward: bezelc SIGSEGV (exit 139).
BEZEL_TEST(codegen_issue_5319,
           generic_owner_recursive_computed_property_int_and_long_spec_23_2_14_1) {
    std::string out = runLoop200(
        "module M5319P2j; "
        "class Box5319P2j<T> "
        "  private _v: T; "
        "  private _next: Box5319P2j<T>?; "
        "  public constructor(v: T, next: Box5319P2j<T>?) _v := v; _next := next; end constructor; "
        // §23.2 / §14.1: the same terminating chain-walk as T1, with `T`
        // threaded through the value, the null test, and the field it walks.
        "  public property Rec: T => (if _next = null then _v else _next!.Rec); "
        "end Box5319P2j; "
        "class C "
        "  public static method Main(): int "
        "    var fails := 0; "
        "    for var k in 0..<200 do "
        "      using var bi := new Box5319P2j<int>(1, new Box5319P2j<int>(2, new Box5319P2j<int>(3, null))); "
        "      if bi.Rec <> 3 then fails := fails + 1; end if; "
        // The SECOND instantiation is the load-bearing half: `T` is 8 bytes
        // here, so the same body that answered `bi` reads `_next` at the wrong
        // offset when handed `bl`.
        "      using var bl := new Box5319P2j<long>(4L, new Box5319P2j<long>(5L, new Box5319P2j<long>(6L, null))); "
        "      if bl.Rec <> 6L then fails := fails + 1; end if; "
        "    end for; "
        "    Console.WriteLine(fails); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, "0");
}

// ── T10 — same shape, but the second instantiation is `string`. A POINTER-
// sized `T` erases to the same `ptr` as a class reference, which is precisely
// why this row is not redundant with T9: the erased template's field offsets
// and the two instantiations' disagree in a different pattern, and the failure
// surfaces in LLVM IR VERIFICATION rather than as a runtime SEGV. Pre-fix-
// forward: "PHI node operands are not the same type".
BEZEL_TEST(codegen_issue_5319,
           generic_owner_recursive_computed_property_int_and_string_spec_23_2_14_1) {
    std::string out = runLoop200(
        "module M5319P2f; "
        "class Box5319P2f<T> "
        "  private _v: T; "
        "  private _next: Box5319P2f<T>?; "
        "  public constructor(v: T, next: Box5319P2f<T>?) _v := v; _next := next; end constructor; "
        "  public property Rec: T => (if _next = null then _v else _next!.Rec); "
        "end Box5319P2f; "
        "class C "
        "  public static method Main(): int "
        "    var fails := 0; "
        "    for var k in 0..<200 do "
        "      using var bi := new Box5319P2f<int>(1, new Box5319P2f<int>(2, new Box5319P2f<int>(3, null))); "
        "      if bi.Rec <> 3 then fails := fails + 1; end if; "
        "      using var bs := new Box5319P2f<string>(\"a\", new Box5319P2f<string>(\"b\", new Box5319P2f<string>(\"c\", null))); "
        "      if bs.Rec <> \"c\" then fails := fails + 1; end if; "
        "    end for; "
        "    Console.WriteLine(fails); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, "0");
}

// ── T11 — same shape with `bool`. The third distinct `T` LOWERING: `i1`-vs-
// `i32` width in the erased template, and a `bool` property whose getter
// returns a value whose representation the wrong-layout body would load from
// the wrong slot. Three instantiations across T9–T11 (int/long, int/string,
// int/bool) is what makes the row set a claim about PER-INSTANTIATION
// specialization rather than about one lucky second type.
BEZEL_TEST(codegen_issue_5319,
           generic_owner_recursive_computed_property_int_and_bool_spec_23_2_14_1) {
    std::string out = runLoop200(
        "module M5319P2h; "
        "class Box5319P2h<T> "
        "  private _v: T; "
        "  private _next: Box5319P2h<T>?; "
        "  public constructor(v: T, next: Box5319P2h<T>?) _v := v; _next := next; end constructor; "
        "  public property Rec: T => (if _next = null then _v else _next!.Rec); "
        "end Box5319P2h; "
        "class C "
        "  public static method Main(): int "
        "    var fails := 0; "
        "    for var k in 0..<200 do "
        "      using var bi := new Box5319P2h<int>(1, new Box5319P2h<int>(2, new Box5319P2h<int>(3, null))); "
        "      if bi.Rec <> 3 then fails := fails + 1; end if; "
        "      using var bb := new Box5319P2h<bool>(true, new Box5319P2h<bool>(false, new Box5319P2h<bool>(true, null))); "
        "      if bb.Rec <> true then fails := fails + 1; end if; "
        "    end for; "
        "    Console.WriteLine(fails); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, "0");
}
