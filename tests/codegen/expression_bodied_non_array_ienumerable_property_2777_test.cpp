// Codegen / SIGSEGV — #2777 / §29.2 / §23.2: an EXPRESSION-bodied
// (`=> expr;`) `IEnumerable<T>`-declared accessor whose getter body yields a
// NON-ARRAY-backed collection (`Set<T>`, `OwningSet<T>`, `LinkedList<T>`, or
// a user class implementing IEnumerable<T>) lowered the raw collection
// pointer instead of a vtable-backed enumerator. Every consumer of the
// property read — a local-decl-init / a direct for-in — then drove §29.2
// vtable dispatch on the raw descriptor and jumped through vtable slot 4
// (GetEnumerator, NULL) → SIGSEGV (`pc 0x0`, exit 139).
//
// #2703 fixed the BLOCK-BODIED sibling by applying the §29.2 wrap on the
// RETURN edge (`wrapAsIEnumerableForTarget` in `lowerReturn`); #2815
// extended the EXPRESSION-BODIED form to the ARRAY-family getter body
// (`wrapPropertyArrayAsIEnumerable`). Neither covers the non-array source
// families the BLOCK form's RETURN path covers, so a Set/LinkedList/user-
// class getter body still crashed. This commit calls
// `wrapAsIEnumerableForTarget` from the EXPRESSION-BODIED emit site
// (mirroring the BLOCK form's RETURN-edge call verbatim — same parameters,
// same helper), so every `wrapAsIEnumerableForTarget` arm fires at the
// EXPRESSION-BODIED read too. IOwningEnumerable+Array stays on the narrow
// `wrapPropertyArrayAsIEnumerable` path (unchanged); IOwningEnumerable+
// non-Array is out-of-scope (no wrap variant exists today; see the follow-
// up note in the commit message).
//
// runAotChecked links + runs under ASan/LSan; the SEGV on the pre-fix path
// is caught as a non-zero exit code (or ASan's "SEGV on unknown address
// 0x000000000000 (pc 0x0)"). The exit code pins the correct enumeration
// result (64 × (3+4) = 448), so a UAF in the wrapper would corrupt the
// yield values as well. Each cell below names the exact §23.2 / §29.2
// wiring it exercises so a future ratchet review can identify the
// covered surface. // per §29.2, §23.2, §28.9 (issue #2777)

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// // per §29.2, §23.2 (issue #2777)
// ---------------------------------------------------------------------------

// HEADLINE: the issue's repro, retargeted to `OwningSet<Item>` instead of
// `OwningArray<Item>` (the Array case is covered by #2815). A property
// `E: IEnumerable<Item>` whose EXPRESSION-bodied getter yields an
// `OwningSet<Item>` field. `OwningSet<T>` requires `T: class`, so the set
// is bz_array_t-backed (the same storage the Array family uses) and the
// block-bodied form's `wrapAsIEnumerableForTarget` already wraps this
// shape on the RETURN edge (#2703). The expression-bodied form lacks the
// RETURN edge and was wrapping only the Array family (#2815), so a Set
// getter body fell through unwrapped and the for-in jumped through a NULL
// vtable slot. Pre-fix: pc 0x0 SIGSEGV. Post-fix: exit code 448, LSan-
// clean (the wrapper frame is disposed by the for-in's own iterEnumDispose
// — see #1840/#2485: !sourceIsNamedLocal → COMPUTED, fresh-value case).
BEZEL_TEST(expression_bodied_non_array_ienumerable_property_2777,
           set_class_element_getter_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _s: OwningSet<Item>; "
        "  public constructor() "
        "    _s := new OwningSet<Item>(); "
        "    _s.Add(new Item(3)); "
        "    _s.Add(new Item(4)); "
        "  end constructor; "
        "  public property E: IEnumerable<Item> "
        "    get => _s; "
        "  end E; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var i in 1..64 do "
        "    for var x in h.E do total := total + x.V; end; "
        "  end; "
        "  if total = 448 then return 58; end; "
        "  return 1; "
        "end Main; end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_TRUE(r.ran);    // ASan-clean (wrapper frame freed exactly once)
    BEZEL_EXPECT_EQ(r.exitCode, 58);  // 448 = 58 in the issue's exit-code mapping
}

// LOCAL-INIT CELL: `var e := h.E;` then `for var x in e do …`. The local-
// init path (lowerLocalDecl) does NOT double-wrap the property-read value
// (the property read returns a vtable-backed enumerator and the local is
// pure IEnumerable<T>); the for-in's own iterEnumDispose at scope end
// frees the wrapper frame (matches #1840/#2485's !sourceIsNamedLocal +
// COMPUTED-fresh-value classification for a property-source local).
// Pre-fix: SIGSEGV at slot 4. Post-fix: exit 58, clean.
BEZEL_TEST(expression_bodied_non_array_ienumerable_property_2777,
           set_class_element_local_init_then_for_in_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _s: OwningSet<Item>; "
        "  public constructor() "
        "    _s := new OwningSet<Item>(); "
        "    _s.Add(new Item(3)); "
        "    _s.Add(new Item(4)); "
        "  end constructor; "
        "  public property E: IEnumerable<Item> "
        "    get => _s; "
        "  end E; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var i in 1..64 do "
        "    var e := h.E; "
        "    for var x in e do total := total + x.V; end; "
        "  end; "
        "  if total = 448 then return 58; end; "
        "  return 1; "
        "end Main; end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 58);
}

// BARE-IDENTIFIER CELL: `for var x in E` inside the declaring class where
// `E` is the property `=> _s`. Exercises the bare-`this` property-read
// path (LlvmBackend_Exprs.cpp ~6050) — a SEPARATE site from the
// qualified-`obj.P` path the headline exercises; both must wrap.
// Pre-fix: SIGSEGV. Post-fix: exit 58, clean.
BEZEL_TEST(expression_bodied_non_array_ienumerable_property_2777,
           set_class_element_bare_identifier_read_in_class_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _s: OwningSet<Item>; "
        "  public constructor() "
        "    _s := new OwningSet<Item>(); "
        "    _s.Add(new Item(3)); "
        "    _s.Add(new Item(4)); "
        "  end constructor; "
        "  public property E: IEnumerable<Item> "
        "    get => _s; "
        "  end E; "
        "  public method Sum(): int "
        "    var total := 0; "
        "    for var x in E do total := total + x.V; end; "
        "    return total; "
        "  end Sum; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var i in 1..64 do total := total + h.Sum(); end; "
        "  if total = 448 then return 58; end; "
        "  return 1; "
        "end Main; end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 58);
}

// CONTROL: the BLOCK-BODIED sibling for the SAME OwningSet<Item> field.
// The block form has been correct since #2703 — its RETURN-edge wrap
// (wrapAsIEnumerableForTarget) handles Set/Array/LinkedList/user-class
// sources uniformly. Pins that the issue is the EXPRESSION-BODIED form's
// wrap surface, NOT the wrap itself; green on both pre- and post-fix to
// prove the regression test isolates the EXPRESSION-BODIED defect.
// // per §29.2, §23.2
BEZEL_TEST(expression_bodied_non_array_ienumerable_property_2777,
           set_class_element_block_bodied_control_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _s: OwningSet<Item>; "
        "  public constructor() "
        "    _s := new OwningSet<Item>(); "
        "    _s.Add(new Item(3)); "
        "    _s.Add(new Item(4)); "
        "  end constructor; "
        "  public property E: IEnumerable<Item> "
        "    get => do return _s; end; "
        "  end E; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var i in 1..64 do "
        "    for var x in h.E do total := total + x.V; end; "
        "  end; "
        "  if total = 448 then return 58; end; "
        "  return 1; "
        "end Main; end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 58);
}