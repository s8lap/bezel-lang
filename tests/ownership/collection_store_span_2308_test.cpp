// Spec-derived tests for issue #2308 — §9.7 (v0.226) collection-store bullet,
// the sibling of the #1251 instance-field fix.
//
// §9.7 reads: "A borrow-bearing record value may be **copied into a
// collection** only when every carried borrow's source provably outlives the
// collection's own binding scope." §9.7 also says: "The check is the same
// structural scope comparison as §28.3's borrow rules; there is **no trust
// residual for record-carried borrows** (same reasoning as the instance-field
// bullet above)." The instance-field bullet was fixed by #1251 (it now
// classifies the RHS with `isBorrowBearingRecordOrSpan` and emits a
// span-flavoured diagnostic on the builtin span leg). The "copied into a
// collection" bullet was NOT touched by that fix: the
// `MethodOwnership::checkCollectionBorrowStoreEscape` `RecvKind::Escapes`
// leg consulted only `isBorrowBearingRecord` (and later
// `isBorrowBearingChoice` for #2209), both of which require a user Decl —
// the builtin spans (`Span<T>` / `ReadOnlySpan<T>` / `ArraySpan<T>` /
// `ReadOnlyArraySpan<T>`, stdlib §4.14/§4.15) have none, so a bare span
// element slipped the check. The store compiled clean and the runtime
// SEGV'd once the backing buffer was disposed (verified: rc=0 at compile,
// ASan heap-use-after-free in `bz_array_get` at run time).
//
// Fix: extend the Escapes leg with a span predicate
// (`isBorrowBearingRecordOrSpan`) and emit a span-flavoured diagnostic on
// the span branch. The "no trust residual" wording in §9.7 — same for
// records, choices, and spans — keeps the leg TYPE-based and unconditional
// on the Escapes receiver: a parameter-sourced record/choice/span into a
// field-typed collection IS rejected (no trust residual). The only
// "exemption" is the null-source form (a record/choice whose every
// borrow-carrying component is null), which a span has no instance of (a
// span's backing-store field is non-nullable per §4.14/§4.15) — so no
// exemption applies on the span leg, matching the #1251 instance-field
// precedent.
//
// This file pins the negative (REJECT) shapes — the exact issue repro plus
// the other store sinks listed in §28.11's `storing` whitelist
// (`Add` / `Insert` / `Push`) and the §4.15 sibling — and the positive
// (ACCEPT) shapes: a method-local pair (the receiver is a method-local
// collection, classified `RecvKind::Local` rather than `RecvKind::Escapes`,
// so the TYPE-based Escapes leg does not fire; the general borrow-source
// scope comparison at the Local leg admits the same-scope pair).
//
// per §9.7 (v0.226), §4.14, §4.15, §28.11

#include <string>

#include "support/ownership_support.hpp"  // parseAndCheckOwnership — sema + ownership
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

// §9.7 / §4.14 / §4.15 (v0.226, issue #2308): the collection-store span
// diagnostic. The diagnostic names the ROOT identifier of the receiver
// expression (the MemberAccessExpr's outermost identifier — `h` in
// `h.spans.Add(...)`, `_arr` in the bare `_arr.Add(...)` form) — mirrors
// `recvRoot` in checkCollectionBorrowStoreEscape. Distinct from #1251's
// field-store span diagnostic: this one names the "copied into a
// collection" bullet and the store sink (`.Add` / `.Insert` / …). The
// null-source form is NOT an admitted exemption, so the recognizer does
// not require a `null` substring.
static bool rejectsCollectionStoreSpan(const OwnershipResult& r,
                                       std::string_view recvRootName,
                                       std::string_view sinkName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("§9.7") == std::string::npos) continue;
        if (d.message.find("§4.14") == std::string::npos &&
            d.message.find("§4.15") == std::string::npos)
            continue;
        if (d.message.find(std::string(sinkName)) == std::string::npos)
            continue;
        if (d.message.find(std::string(recvRootName)) == std::string::npos)
            continue;
        // The collection-store diagnostic names the receiver as a collection
        // ("copied into a collection"). Pin that wording so a misfire
        // against the #1251 instance-field diagnostic ("an instance field
        // of a class") is detected.
        if (d.message.find("collection") == std::string::npos) continue;
        return true;
    }
    return false;
}

namespace {
const char* kCommon =
    "module M; "
    "class C "
    "  static method MakeBuf(): OwningBuffer<int> "
    "    var b := new OwningBuffer<int>(4); "
    "    b[0] := 77; "
    "    return b; "
    "  end MakeBuf; "
    "end C; ";
std::string mod(const std::string& decls) {
    return std::string(kCommon) + decls;
}
}  // namespace

// ===========================================================================
// NEGATIVE — a bare span RHS copied into a class-field-typed collection is
// rejected. The receiver root is a class field (the `MemberAccessExpr`'s
// outer identifier is the parameter `h`; its `.spans` field classifies the
// receiver as `RecvKind::Escapes`). The span RHS has no null-source form
// (§4.14/§4.15: the backing-store field is non-nullable), so §9.7's
// "no trust residual" wording rejects unconditionally.
// ===========================================================================

// The exact shape the issue names: `h.spans.Add(buf.AsSpan())` where `buf`
// is a method-scope `using var OwningBuffer<int>` and `spans` is a class
// instance field. Before the fix this compiled clean and SEGV'd at runtime
// (verified: ASan SEGV in bz_array_get via bz$M.Holder$Read). The receiver
// root is `h`.
BEZEL_TEST(collection_store_span_2308, field_array_add_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned spans: Array<Span<int>>; "
        "  public constructor() spans := new Array<Span<int>>(); end constructor; "
        "end Holder; "
        "class C "
        "  static method Fill(h: Holder) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 77; "
        "    h.spans.Add(buf.AsSpan()); "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionStoreSpan(r, "h", "Add"));
}

// The Insert sink: same receiver, the §28.11 whitelist's second entry
// (Array<T>.Insert is the borrowing-family Insert — §4.4). Pinning per
// sink pins the `storing` whitelist match path the
// `MethodOwnership::checkCollectionBorrowStoreEscape` entrypoint gates on.
BEZEL_TEST(collection_store_span_2308, field_array_insert_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned spans: Array<Span<int>>; "
        "  public constructor() spans := new Array<Span<int>>(); end constructor; "
        "end Holder; "
        "class C "
        "  static method Fill(h: Holder) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 77; "
        "    h.spans.Insert(0, buf.AsSpan()); "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionStoreSpan(r, "h", "Insert"));
}

// The Push sink — the Stack-family entry. Exercises the third bucket of
// the `storing` whitelist to pin the match is sink-name-correct, not just
// "Add-shaped".
BEZEL_TEST(collection_store_span_2308, field_stack_push_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned spans: Stack<Span<int>>; "
        "  public constructor() spans := new Stack<Span<int>>(); end constructor; "
        "end Holder; "
        "class C "
        "  static method Fill(h: Holder) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 77; "
        "    h.spans.Push(buf.AsSpan()); "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionStoreSpan(r, "h", "Push"));
}

// The ArraySpan / ReadOnlyArraySpan family — §4.15 — the OTHER side of the
// builtin-span taxonomy. The fix is per-family: every span kind must fire.
BEZEL_TEST(collection_store_span_2308, field_array_arrayspan_add_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned spans: Array<ArraySpan<int>>; "
        "  public constructor() spans := new Array<ArraySpan<int>>(); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  static method Fill(h: Holder) "
        "    var local := new Array<int>(); "
        "    local.Add(1); "
        "    h.spans.Add(local.AsSpan()); "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionStoreSpan(r, "h", "Add"));
}

// A span LVALUE produced by a method call (`C.MakeBuf().AsSpan()`) — the
// source is a method-result temporary, but §9.7's "no trust residual"
// rule is TYPE-based and fires regardless of the source's provenance. The
// §9.7 "construction from a temporary" sibling also fires (on a separate
// path, for the span CONSTRUCTION itself), but the new leg fires first
// for the field-typed Escapes receiver.
BEZEL_TEST(collection_store_span_2308, field_array_temporary_source_add_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned spans: Array<Span<int>>; "
        "  public constructor() spans := new Array<Span<int>>(); end constructor; "
        "end Holder; "
        "class C "
        "  static method MakeBuf(): OwningBuffer<int> "
        "    var b := new OwningBuffer<int>(4); b[0] := 99; return b; "
        "  end MakeBuf; "
        "  static method Fill(h: Holder) "
        "    h.spans.Add(C.MakeBuf().AsSpan()); "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionStoreSpan(r, "h", "Add"));
}

// A bare-identifier field-typed collection: the receiver is the field
// itself (a `Class` IdentifierExpr, not a MemberAccessExpr), so the
// receiver-classification still tags it `RecvKind::Escapes` (the bare-
// field leg). Pins the Escapes branch fires for the bare-identifier form
// too, not just the qualified `obj.f.Add` form.
BEZEL_TEST(collection_store_span_2308, bare_field_array_add_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned spans: Array<Span<int>>; "
        "  public constructor() spans := new Array<Span<int>>(); end constructor; "
        "  public method Stash() "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 7; "
        "    spans.Add(buf.AsSpan()); "
        "  end Stash; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionStoreSpan(r, "spans", "Add"));
}

// ===========================================================================
// POSITIVE — no over-rejection.
// ===========================================================================

// A same-scope local pair: `coll.Add(s)` where `coll` is a method-local
// collection and `s` is a method-local span bound to a `using var buf` in
// the same scope. The receiver is a method-local (RecvKind::Local, not
// Escapes — the bare-identifier local leg), and the general borrow-source
// scope comparison at the Local leg admits the same-scope pair (the
// source and the receiver die at the same scope). The new TYPE-based
// Escapes leg does NOT fire here — it gates on `kind == RecvKind::Escapes`
// — confirming the new leg does not widen to the Local receiver class.
// This is the "lifetime-correct" shape the issue's note describes: the
// source provably outlives the collection's own binding scope (same scope
// = same outlives).
BEZEL_TEST(collection_store_span_2308, same_scope_local_pair_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 11; "
        "    var coll := new Array<Span<int>>(); "
        "    var s := buf.AsSpan(); "
        "    coll.Add(s); "
        "    return coll[0][0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (The owning-family `Add` takes an OWNED argument and is excluded from the
// §28.11 `borrowingFamily` whitelist at the outer gate of
// `checkCollectionBorrowStoreEscape` — `OwningArray<…>`, `OwningDictionary<…>`
// are absent. The new span leg is on the Escapes leg of that same gate, so
// an owning-family store is filtered out before reaching it. This is
// structurally guaranteed, not asserted here, because exercising it cleanly
// requires the `M(owned …)` transfer-call syntax (§22.3) — an unrelated
// concern from the borrow-store rule under test.)

// The non-store method (Count, Length, Contains, …) is not in the
// `storing` whitelist, so the function early-returns at the entrypoint
// and neither the type-based leg nor the general borrow-source check
// fires. Pinned to confirm the new leg does not widen the entrypoint
// gate to non-store methods.
BEZEL_TEST(collection_store_span_2308, non_store_method_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public owned spans: Array<Span<int>>; "
        "  public constructor() spans := new Array<Span<int>>(); end constructor; "
        "end Holder; "
        "class C "
        "  static method Count(h: Holder): int return h.spans.Count; end Count; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
