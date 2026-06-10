// #600 contract — §30.14 sendable types, value-type category.
//
// These tests translate §30.14 "Sendable types" (value-type sub-rules) and
// §30.15 "Channels" (element-type restriction) into sema-level diagnostics.
// They are EXPECTED TO FAIL against current main: sema has no knowledge of
// Channel<T>, Domain.Spawn, or the sendable derived type property (§3.6/§30.14).
// The implementation lane re-enables them by removing the commented-out entries
// in tests/CMakeLists.txt.
//
// Spec derivation:
//   §30.14 "Sendable types" — value type rule:
//     "Value types are sendable iff they are not borrow-bearing (§9.7) and
//      not move-only (§15.3)."
//   §3.6 — diagnostics name the property ("not sendable"), the offending type,
//     and the failing rule.
//   §30.15 — Channel<T> element type must be sendable; checked at instantiation.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff any diagnostic mentions "not sendable" (the spec-prescribed text,
// §3.6/§30.14: "diagnostics name the property ('not sendable')").
bool hasNotSendableDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not sendable") != std::string::npos ||
            d.message.find("sendable") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// §30.14 Value types — plain records (no borrow-bearing fields) ARE sendable.
// §30.15 Channel<T> with a plain-value T must be accepted.
// ---------------------------------------------------------------------------

// §30.14: A record with only primitive fields is not borrow-bearing (§9.7) and
// not move-only (§15.3) → sendable → Channel<Point> is valid.
// This test expects NO error. Against current main it will fail because Channel
// is an unknown type.
BEZEL_TEST(send_600_value_types,
           plain_record_as_channel_element_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "record Point public X: int := 0; public Y: int := 0; end Point; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Point>(); "
        "  end Test; "
        "end C;");
    // Accepted: Point is sendable (non-borrow-bearing, non-move-only value type).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: primitives (int, bool, etc.) are sendable. Channel<int> must be
// accepted with no error.
BEZEL_TEST(send_600_value_types,
           primitive_as_channel_element_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: string is sendable (crosses as shared immutable reference).
// Channel<string> must be accepted.
BEZEL_TEST(send_600_value_types,
           string_as_channel_element_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<string>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: a record with a class-typed (reference-typed) field is borrow-bearing
// (§9.7) → NOT sendable → Channel<BorrowRec> is rejected with "not sendable".
BEZEL_TEST(send_600_value_types,
           borrow_bearing_record_as_channel_element_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Node end Node; "
        // 'ref' field of class type makes it borrow-bearing per §9.7
        "record BorrowRec public Ref: Node?; end BorrowRec; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<BorrowRec>(); "
        "  end Test; "
        "end C;");
    // Rejected: BorrowRec is borrow-bearing (class-typed field) → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14/§15.3: move-only value types ([N owned]T) are NOT sendable.
// Channel<[2 owned]Node> must be rejected.
BEZEL_TEST(send_600_value_types,
           move_only_value_type_as_channel_element_rejected_30_14_15_3) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Node "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Node; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<[2 owned]Node>(); "
        "  end Test; "
        "end C;");
    // Rejected: [N owned]T is move-only → not sendable per §30.14/§15.3.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14: CancellationToken is explicitly listed as borrow-bearing record
// → not sendable (spec §30.14: "CancellationToken is a borrow-bearing record
// and therefore not sendable by the value-type rule").
// Channel<CancellationToken> must be rejected.
BEZEL_TEST(send_600_value_types,
           cancellation_token_as_channel_element_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<CancellationToken>(); "
        "  end Test; "
        "end C;");
    // Rejected: CancellationToken is borrow-bearing → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14: a record with only string fields is NOT borrow-bearing (string is
// runtime-managed, §9.7) → sendable. Channel<NamedRecord> is accepted.
BEZEL_TEST(send_600_value_types,
           record_with_only_string_fields_sendable_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "record NamedRecord public Name: string; public Tag: string; end NamedRecord; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<NamedRecord>(); "
        "  end Test; "
        "end C;");
    // Accepted: string fields don't make a record borrow-bearing (§9.7).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: function-typed values are never sendable ("Function-typed values are
// never sendable"). A record with a function field is borrow-bearing → rejected.
BEZEL_TEST(send_600_value_types,
           record_with_function_field_as_channel_element_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "record FuncRec public Callback: (int) -> int; end FuncRec; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<FuncRec>(); "
        "  end Test; "
        "end C;");
    // Rejected: function-typed field. Function-typed values are never sendable
    // per §30.14.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14: borrow-bearing record as a spawn arg is also rejected.
// Domain.Spawn with a borrow-bearing arg type must fail.
BEZEL_TEST(send_600_value_types,
           borrow_bearing_record_as_spawn_arg_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Node end Node; "
        "record BorrowArg public Ref: Node?; end BorrowArg; "
        "class Worker "
        "  public static method Run(arg: BorrowArg) end Run; "
        "end Worker; "
        "class Main "
        "  public static method Go() "
        "    var arg := new BorrowArg(); "
        "    using var t := Domain.Spawn(Worker.Run, arg); "
        "  end Go; "
        "end Main;");
    // Rejected: BorrowArg is borrow-bearing → not sendable as spawn argument.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}
