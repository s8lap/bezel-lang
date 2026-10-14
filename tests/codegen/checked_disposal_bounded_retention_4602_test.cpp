// Codegen: checked disposal keeps only the blocks a surviving borrow can reach
// (issue #4602, layer 1 — spec §28.2 v0.338, "Use after disposal; checked
// disposal mode").
//
// §28.2 v0.338: checked mode is the default build, and "the implementation may
// quarantine" a disposed block so that a member access through a surviving
// borrow raises DisposedAccessException deterministically. Quarantining EVERY
// disposed block makes a create-and-dispose loop grow without bound (MEASURED:
// 10^7 iterations of a destructor class, 316 MB peak RSS checked vs 4 MB
// unchecked). Layer 1 frees the block at the scope-end release of a fresh,
// never-reassigned local used only at borrow positions, unless a position that
// can retain it past the frame — a store of `this`, an argument, a lambda
// capture, a bound method reference — marked it escaped at runtime.
//
// Rows:
//   1-5. An escaped borrow read after the owner's release still faults, for each
//        retention kind: `this` passed from a constructor to a storing callee,
//        `this` passed from a method, a lambda capturing `this`, a bound method
//        reference, and `this` added to a borrowing collection (§28.3's
//        borrowed-field store rule / §28.11 — the K4.3 trust-admitted stores).
//        Each runs plain checked AND under generated-code ASan: a missing mark
//        frees the block and the ASan leg reports the use-after-free.
//   6.   An unescaped fresh local's block is freed: the ASan/LSan leg finds no
//        leak (before this fix every one of the 200 blocks was quarantined and
//        leaked — fail-without).
//   7.   K5.6: a borrow of an owned field's displaced value still faults after
//        the overwrite (field releases keep the quarantine).
//   8.   An explicit Dispose with a surviving alias still faults (conformance
//        s28_2_0011's shape).
//   9-16. `this` stored into a borrowing collection from every instance body
//        kind whose bare `this` sema leaves untyped: an indexer getter, a
//        block-bodied property getter, a property setter, a destructor, an
//        extension method, a generic class's method, and through the
//        pass-through parents `(if b then this else this)` and `(this)!`.
//        A missing mark frees the block; the ASan leg then reports the
//        heap-use-after-free on the read (review of the first cut: 50/50 UAF
//        for each of these eight, base: 50/50 DisposedAccessException).
//
// The 2,000,000-iteration RSS bound and the test-only
// BEZEL_CHECKED_DISPOSAL_QUARANTINE_ALL knob are pinned by
// tests/lit/checked_disposal_bounded_retention_4602.bzl.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;
using bezel::test::runAotCheckedImpl;
using bezel::test::JitResult;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Checked-disposal mode, no sanitizer: the observable §28.2 behaviour.
std::string checkedOut(const std::string& src) {
    auto r = runAotCheckedDisposal(src);
    if (!r.ran) return "<did not run>";
    return r.stdoutText;
}

// Checked-disposal mode under generated-code ASan, leaks not checked (an
// escaped block is quarantined by design). A freed-then-read block is a
// heap-use-after-free report, which surfaces as ran=false.
std::string checkedAsanOut(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    auto r = runAotCheckedImpl(src, "runAotCheckedLenient",
                               /*detectLeaks=*/false, opts);
    if (wasSkipped(r)) return "<skip>";
    if (!r.ran) return "<did not run>";
    return r.stdoutText;
}

const char* kRegistry =
    "module M;\n"
    "public class Registry\n"
    "  private _w: Widget?;\n"
    "  private owned _fns: Array<() -> string>;\n"
    "  private owned _items: Array<Widget>;\n"
    "  public constructor()\n"
    "    _w := null;\n"
    "    _fns := new Array<() -> string>();\n"
    "    _items := new Array<Widget>();\n"
    "  end constructor;\n"
    "  public method Keep(w: Widget)\n"
    "    _w := w;\n"
    "  end Keep;\n"
    "  public method Read(): string => _w!.GetLabel();\n"
    "  public method Fns(): Array<() -> string> => _fns;\n"
    "  public method Items(): Array<Widget> => _items;\n"
    "end Registry;\n"
    "public class Widget\n"
    "  public property Label: string;\n"
    "  public constructor()\n"
    "    Label := \"live\";\n"
    "  end constructor;\n"
    "  public constructor(r: Registry)\n"
    "    Label := \"live\";\n"
    "    r.Keep(this);\n"
    "  end constructor;\n"
    "  public method Attach(r: Registry)\n"
    "    r.Keep(this);\n"
    "  end Attach;\n"
    "  public method Hook(r: Registry)\n"
    "    r.Fns().Add(lambda(): string => GetLabel());\n"
    "  end Hook;\n"
    "  public method HookBound(r: Registry)\n"
    "    r.Fns().Add(GetLabel);\n"
    "  end HookBound;\n"
    "  public method Enlist(r: Registry)\n"
    "    r.Items().Add(this);\n"
    "  end Enlist;\n"
    "  public method GetLabel(): string => Label;\n"
    "  destructor\n"
    "  end destructor;\n"
    "end Widget;\n";

// `body` runs inside `do … end` with `reg` in scope; the owner `w` is released
// at the block's end, then `probe` reads through the escaped borrow.
std::string escapeProgram(const std::string& body, const std::string& probe) {
    return std::string(kRegistry) +
        "public class Program\n"
        "  public static method Main()\n"
        "    using var reg := new Registry();\n"
        "    do\n" + body +
        "    end;\n"
        "    try\n"
        "      Console.WriteLine(" + probe + ");\n"
        "    catch\n"
        "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
        "    end try;\n"
        "  end Main;\n"
        "end Program;\n";
}

void expectEscapedFaults(const std::string& src) {
    BEZEL_EXPECT_EQ(checkedOut(src), std::string("faulted\n"));
    std::string asan = checkedAsanOut(src);
    if (asan != "<skip>") BEZEL_EXPECT_EQ(asan, std::string("faulted\n"));
}

// Rows 9-16: `widgetMembers` is spliced into a Widget that borrows `_reg`;
// `extraDecls` follows Program. Sixty create-and-release cycles each run
// `use` on a fresh `w := <newExpr>`, so a single missing mark is a freed block
// that the final read dereferences.
std::string thisEscapeProgram(const std::string& widgetMembers,
                              const std::string& use,
                              const std::string& newExpr = "new Widget(reg)",
                              const std::string& extraDecls = "") {
    return std::string(
        "module M;\n"
        "public class Registry\n"
        "  private owned _items: Array<Widget>;\n"
        "  public constructor()\n"
        "    _items := new Array<Widget>();\n"
        "  end constructor;\n"
        "  public method Items(): Array<Widget> => _items;\n"
        "end Registry;\n"
        "public class Widget\n"
        "  private _reg: Registry;\n"
        "  public property Label: string;\n"
        "  public constructor(r: Registry)\n"
        "    _reg := r;\n"
        "    Label := \"live\";\n"
        "  end constructor;\n"
        "  public method GetLabel(): string => Label;\n") +
        widgetMembers +
        "end Widget;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var reg := new Registry();\n"
        "    var i := 0;\n"
        "    while i < 60 do\n"
        "      do\n"
        "        var w := " + newExpr + ";\n"
        "        " + use + "\n"
        "      end;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    try\n"
        "      Console.WriteLine(reg.Items()[0].GetLabel());\n"
        "    catch\n"
        "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
        "    end try;\n"
        "  end Main;\n"
        "end Program;\n" + extraDecls;
}

const char* kDestructor = "  destructor\n  end destructor;\n";

}  // namespace


// ── 1. `this` handed from a constructor to a storing callee ─────────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_ctor_this_store_read_after_release_faults) {
    // §28.3 borrowed-field store rule (K4.3 trust clause): the constructor
    // passes `this` to Registry.Keep, which stores it. `w` is a fresh,
    // never-reassigned local used only as a receiver — free-eligible — so only
    // the escape mark on the `this` argument keeps its block readable.
    expectEscapedFaults(escapeProgram(
        "      var w := new Widget(reg);\n"
        "      _ := w.Label;\n",
        "reg.Read()"));
}


// ── 2. `this` handed from a method to a storing callee ──────────────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_method_this_store_read_after_release_faults) {
    expectEscapedFaults(escapeProgram(
        "      var w := new Widget();\n"
        "      w.Attach(reg);\n",
        "reg.Read()"));
}


// ── 3. a lambda capturing `this`, stored in a borrowing collection ──────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_lambda_capture_read_after_release_faults) {
    // §27.5: the env holds `this` past the frame; the capture is marked at
    // env construction.
    expectEscapedFaults(escapeProgram(
        "      var w := new Widget();\n"
        "      w.Hook(reg);\n",
        "reg.Fns()[0]()"));
}


// ── 4. a bound method reference (§27.6) ─────────────────────────────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_bound_method_receiver_read_after_release_faults) {
    // §27.6: `GetLabel` as a value binds the implicit `this` into the env.
    expectEscapedFaults(escapeProgram(
        "      var w := new Widget();\n"
        "      w.HookBound(reg);\n",
        "reg.Fns()[0]()"));
}


// ── 5. `this` added to a borrowing collection (§28.11) ──────────────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_borrowing_collection_add_read_after_release_faults) {
    expectEscapedFaults(escapeProgram(
        "      var w := new Widget();\n"
        "      w.Enlist(reg);\n",
        "reg.Items()[0].GetLabel()"));
}


// ── 6. an unescaped fresh local's block is freed ────────────────────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_unescaped_fresh_local_block_is_freed_not_quarantined) {
    // §28.2: quarantine is a latitude for a block a surviving borrow can reach.
    // 200 create-and-dispose cycles, each `w` used only as a receiver. Under
    // LeakSanitizer a quarantined block is an unreachable allocation, so a
    // leak-clean run proves every block was freed (pre-fix: 200 leaked).
    const std::string src =
        "module M;\n"
        "public class Widget\n"
        "  public property Value: int;\n"
        "  public constructor(v: int)\n"
        "    Value := v mod 7;\n"
        "  end constructor;\n"
        "  destructor\n"
        "  end destructor;\n"
        "end Widget;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var total := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      using var w := new Widget(i);\n"
        "      total := total + w.Value;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(total);\n"
        "  end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(checkedOut(src), std::string("594\n"));
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    auto r = runAotChecked(src, opts);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("594\n"));
}


// ── 7. K5.6: a borrow of an overwritten owned field still faults ────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_k5_6_field_overwrite_displaced_borrow_still_faults) {
    // §28.7 / K5.6: fields are not frozen; the displaced value is disposed at
    // the overwrite and a surviving borrow of it faults. A field release keeps
    // the quarantine in layer 1.
    const std::string src =
        "module M;\n"
        "public class Widget\n"
        "  public constructor()\n"
        "  end constructor;\n"
        "  public method Ping(): string => \"live\";\n"
        "  destructor\n"
        "  end destructor;\n"
        "end Widget;\n"
        "public class Holder\n"
        "  private owned _w: Widget;\n"
        "  public constructor()\n"
        "    _w := new Widget();\n"
        "  end constructor;\n"
        "  public method Replace()\n"
        "    var b := _w;\n"
        "    _w := new Widget();\n"
        "    try\n"
        "      Console.WriteLine(b.Ping());\n"
        "    catch\n"
        "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
        "    end try;\n"
        "  end Replace;\n"
        "end Holder;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var h := new Holder();\n"
        "    h.Replace();\n"
        "  end Main;\n"
        "end Program;\n";
    expectEscapedFaults(src);
}


// ── 8. explicit Dispose with a surviving alias still faults ─────────────────

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_explicit_dispose_surviving_alias_still_faults) {
    // Conformance s28_2_0011's shape (§28.2: Dispose is logical disposal; the
    // block stays live until the holder's release point).
    const std::string src =
        "module M;\n"
        "public class Widget\n"
        "  public property Label: string;\n"
        "  public constructor()\n"
        "    Label := \"live\";\n"
        "  end constructor;\n"
        "  destructor\n"
        "  end destructor;\n"
        "end Widget;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var w := new Widget();\n"
        "    var view := w;\n"
        "    w.Dispose();\n"
        "    try\n"
        "      Console.WriteLine(view.Label);\n"
        "    catch\n"
        "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
        "    end try;\n"
        "  end Main;\n"
        "end Program;\n";
    expectEscapedFaults(src);
}


// ── 9. `this` escapes from an indexer getter ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_indexer_getter_this_store_read_after_release_faults) {
    // The bare `this` here carries no sema type; it is marked anyway.
    expectEscapedFaults(thisEscapeProgram(
        std::string(
            "  public property [k: int]: int\n"
            "    get => do _reg.Items().Add(this); return k; end;\n"
            "    set => do _reg.Items().Add(this); end;\n"
            "  end;\n") + kDestructor,
        "_ := w[0];"));
}


// ── 10. `this` escapes from a block-bodied property getter ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_block_property_getter_this_store_read_after_release_faults) {
    // The bare `this` here carries no sema type; it is marked anyway.
    expectEscapedFaults(thisEscapeProgram(
        std::string(
            "  public property Hooked: int\n"
            "    get => do _reg.Items().Add(this); return 1; end;\n"
            "  end;\n") + kDestructor,
        "_ := w.Hooked;"));
}


// ── 11. `this` escapes from a property setter ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_property_setter_this_store_read_after_release_faults) {
    // The bare `this` here carries no sema type; it is marked anyway.
    expectEscapedFaults(thisEscapeProgram(
        std::string(
            "  public property Hook: int\n"
            "    get => 1;\n"
            "    set => do _reg.Items().Add(this); end;\n"
            "  end;\n") + kDestructor,
        "w.Hook := 2;"));
}


// ── 12. `this` escapes from a destructor ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_destructor_this_store_read_after_release_faults) {
    // The bare `this` here carries no sema type; it is marked anyway.
    expectEscapedFaults(thisEscapeProgram(
        "  destructor\n"
        "    _reg.Items().Add(this);\n"
        "  end destructor;\n",
        "_ := w.Label;"));
}


// ── 13. `this` escapes from an extension method ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_extension_method_this_store_read_after_release_faults) {
    // The bare `this` here carries no sema type; it is marked anyway.
    expectEscapedFaults(thisEscapeProgram(
        kDestructor, "w.EnlistX(reg);", "new Widget(reg)",
        "extension Widget\n"
        "  public method EnlistX(r: Registry)\n"
        "    r.Items().Add(this);\n"
        "  end EnlistX;\n"
        "end;\n"));
}


// ── 14. `this` escapes from a generic class's method ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_generic_class_method_this_store_read_after_release_faults) {
    // The bare `this` here carries no sema type; it is marked anyway.
    expectEscapedFaults(thisEscapeProgram(
        kDestructor, "w.EnlistG(reg);", "new GW<int>(reg)",
        "public class GW<T>: Widget\n"
        "  public constructor(r: Registry)\n"
        "    base(r);\n"
        "  end constructor;\n"
        "  public method EnlistG(r: Registry)\n"
        "    r.Items().Add(this);\n"
        "  end EnlistG;\n"
        "end GW;\n"));
}


// ── 15. `this` escapes from a method body: `(if b then this else this)` ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_conditional_this_store_read_after_release_faults) {
    // `(if b then this else this)`: a pass-through parent of an untyped `this`.
    expectEscapedFaults(thisEscapeProgram(
        std::string(
            "  public method Enlist(b: bool)\n"
            "    _reg.Items().Add((if b then this else this));\n"
            "  end Enlist;\n") + kDestructor,
        "w.Enlist(true);"));
}


// ── 16. `this` escapes from a method body: `(this)!` ──

BEZEL_TEST(codegen_checked_disposal_bounded_retention_4602,
           spec_28_2_escaped_postfix_bang_this_store_read_after_release_faults) {
    // `(this)!`: a pass-through parent of an untyped `this`.
    expectEscapedFaults(thisEscapeProgram(
        std::string(
            "  public method Enlist()\n"
            "    _reg.Items().Add((this)!);\n"
            "  end Enlist;\n") + kDestructor,
        "w.Enlist();"));
}
