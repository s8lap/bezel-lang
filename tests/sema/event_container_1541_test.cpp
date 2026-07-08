// Spec-derived sema tests for §24.1 (v0.180, #1541): events may be declared on
// classes and interfaces only.
//
//  §24.1: "Events may be declared on classes and interfaces only (v0.180): the
//  compiler-generated `owned EventSource<T>` backing field requires an owning,
//  disposable container, which records and enums cannot be (§9.7, Appendix C.3)
//  — an `event` member in a record or enum body is a compile error naming this
//  rule."
//
// The rejection is semantic (the shared `Member` grammar production admits the
// parse), fired in the record/enum restriction checks during type resolution.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool diagContains(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §24.1: an `event` member in a RECORD body is a compile error naming the
// owning-disposable-container rule.
BEZEL_TEST(event_container_1541, event_in_record_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "record Bad "
        "  public event Boom: Ev; "
        "end Bad;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "may not declare an `event`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§24.1"));
}

// §24.1: an `event` member in an ENUM body is a compile error naming the same
// rule.
BEZEL_TEST(event_container_1541, event_in_enum_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "enum Color Red, Green, Blue; "
        "  public event Boom: Ev; "
        "end Color;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "may not declare an `event`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§24.1"));
}

// §24.1: a `class` may declare an event (the control case — no error).
BEZEL_TEST(event_container_1541, event_in_class_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Ok "
        "  public event Boom: Ev; "
        "end Ok;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
