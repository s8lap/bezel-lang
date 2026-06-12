// fix #674 — uniform unknown-member diagnostics for compiler-known types.
//
// Spec authority:
//   stdlib §2.16 (v0.135+): Shared<T>, Weak<T>
//   stdlib §6.2  (v0.135+): Mutex
//   stdlib §6.9  (v0.135+): ChannelSender<T>
//   stdlib §4.15 (v0.135+): ArraySpan<T>, ReadOnlyArraySpan<T>
//   stdlib §2.14–2.15:      BinarySerializer
//
// For each type the test suite covers:
//   (a) A phantom member call → compile error ("no member").
//   (b) Each documented member still compiles without error.
//
// Skipped types and reasons are documented at the bottom of this file.

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

bool hasDiag(const bezel::test::SemaResult& r, std::string_view substr) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(substr) != std::string::npos) return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

} // namespace

// ===========================================================================
// Shared<T>  (stdlib §2.16)
// ===========================================================================

// (a) Phantom member on Shared<T> → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           shared_phantom_member_rejected_2_16) {
    // stdlib §2.16 / #674: Shared<T> has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "class Foo end Foo; "
        "class C "
        "  public static method Test(owned s: Shared<Foo>) "
        "    s.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "Shared"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) Documented Shared<T> surface still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           shared_documented_surface_allowed_2_16) {
    // stdlib §2.16: Clone(), Get(), Downgrade(), Dispose() are the full surface.
    auto r = parseSemaAll(
        "module M; "
        "class Foo end Foo; "
        "class C "
        "  public static method Test(owned s: Shared<Foo>) "
        "    using var s2 := s.Clone(); "
        "    var _f := s2.Get(); "
        "    using var w := s2.Downgrade(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ===========================================================================
// Weak<T>  (stdlib §2.16)
// ===========================================================================

// (a) Phantom member on Weak<T> → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           weak_phantom_member_rejected_2_16) {
    // stdlib §2.16 / #674: Weak<T> has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "class Foo end Foo; "
        "class C "
        "  public static method Test(owned w: Weak<Foo>) "
        "    w.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "Weak"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) Documented Weak<T> surface still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           weak_tryupgrade_allowed_2_16) {
    // stdlib §2.16: TryUpgrade(out owned s: Shared<T>?) is the callable surface.
    auto r = parseSemaAll(
        "module M; "
        "class Foo end Foo; "
        "class C "
        "  public static method Test(owned w: Weak<Foo>) "
        "    var ok := w.TryUpgrade(out using var s); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ===========================================================================
// Mutex  (stdlib §6.2)
// ===========================================================================

// (a) Phantom member on Mutex → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           mutex_phantom_member_rejected_6_2) {
    // stdlib §6.2 / #674: Mutex has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var m := new Mutex(); "
        "    m.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "Mutex"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) Mutex.Acquire() still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           mutex_acquire_allowed_6_2) {
    // stdlib §6.2: Acquire() → owned Lock.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var m := new Mutex(); "
        "    using var guard := m.Acquire(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// (b) Mutex.TryAcquire() still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           mutex_tryacquire_allowed_6_2) {
    // stdlib §6.2: TryAcquire(timeout: TimeSpan) → owned Lock?
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var m := new Mutex(); "
        "    using var guard := m.TryAcquire(TimeSpan.FromSeconds(1)); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ===========================================================================
// ChannelSender<T>  (stdlib §6.9)
// ===========================================================================

// (a) Phantom member on ChannelSender<T> → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           channelsender_phantom_member_rejected_6_9) {
    // stdlib §6.9 / #674: ChannelSender<T> has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "ChannelSender"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) ChannelSender<T>.Send() still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           channelsender_send_allowed_6_9) {
    // stdlib §6.9: Send(owned item: T) is the only non-Dispose method.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(42); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ===========================================================================
// ArraySpan<T>  (stdlib §4.15)
// ===========================================================================

// (a) Phantom member on ArraySpan<T> → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           arrayspan_phantom_member_rejected_4_15) {
    // stdlib §4.15 / #674: ArraySpan<T> has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); "
        "    var sp := arr.AsSpan(); "
        "    sp.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) ArraySpan<T> documented surface still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           arrayspan_documented_surface_allowed_4_15) {
    // stdlib §4.15: Length, IsEmpty, IndexOf, Contains, SequenceEqual,
    //   Slice, AsReadOnly, Fill, CopyTo, Reverse all admitted.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); "
        "    var sp := arr.AsSpan(); "
        "    var _len := sp.Length; "
        "    var _emp := sp.IsEmpty; "
        "    var _idx := sp.IndexOf(1); "
        "    var _cnt := sp.Contains(1); "
        "    var _sl := sp.Slice(0, 1); "
        "    var _ro := sp.AsReadOnly(); "
        "    sp.Fill(0); "
        "    sp.Reverse(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// (a) Phantom member on ReadOnlyArraySpan<T> → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           readonlyarrayspan_phantom_member_rejected_4_15) {
    // stdlib §4.15 / #674: ReadOnlyArraySpan<T> has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); "
        "    var sp := arr.AsReadOnlySpan(); "
        "    sp.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) ReadOnlyArraySpan<T> documented surface still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           readonlyarrayspan_documented_surface_allowed_4_15) {
    // stdlib §4.15: Length, IsEmpty, IndexOf, Contains, SequenceEqual,
    //   Slice, CopyTo all admitted on ReadOnlyArraySpan.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); "
        "    var sp := arr.AsReadOnlySpan(); "
        "    var _len := sp.Length; "
        "    var _emp := sp.IsEmpty; "
        "    var _idx := sp.IndexOf(1); "
        "    var _cnt := sp.Contains(1); "
        "    var _sl := sp.Slice(0, 1); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ===========================================================================
// BinarySerializer  (stdlib §2.14–2.15)
// ===========================================================================

// (a) Phantom member on BinarySerializer → compile error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           binaryserializer_phantom_member_rejected_2_15) {
    // stdlib §2.15 / #674: BinarySerializer has no `Bogus` member.
    auto r = parseSemaAll(
        "module M; "
        "import System.Serialization; "
        "class C "
        "  public static method Test() "
        "    using var s := new BinarySerializer(); "
        "    s.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "BinarySerializer"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// (b) BinarySerializer.ToBytes() still compiles without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           binaryserializer_tobytes_allowed_2_15) {
    // stdlib §2.15: ToBytes() → owned OwnedBuffer<byte>.
    auto r = parseSemaAll(
        "module M; "
        "import System.Serialization; "
        "class C "
        "  public static method Test() "
        "    using var s := new BinarySerializer(); "
        "    s.BeginObject(1); "
        "    s.WriteFieldName(\"x\"); "
        "    s.WriteInt(42); "
        "    s.EndObject(); "
        "    using var bytes := s.ToBytes(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// (b) BinarySerializer ISerializer methods still compile without error.
BEZEL_TEST(builtin_674_uniform_unknown_member,
           binaryserializer_iserializer_surface_allowed_2_14) {
    // stdlib §2.14: full ISerializer surface on BinarySerializer must not error.
    auto r = parseSemaAll(
        "module M; "
        "import System.Serialization; "
        "class C "
        "  public static method Test() "
        "    using var s := new BinarySerializer(); "
        "    s.WriteInt(1); "
        "    s.WriteUInt(2); "
        "    s.WriteFloat(3.0); "
        "    s.WriteBool(true); "
        "    s.WriteString(\"hi\"); "
        "    s.WriteChar('A'); "
        "    s.WriteNull(); "
        "    s.BeginArray(0); "
        "    s.EndArray(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ===========================================================================
// Skipped types and rationale (per #674 task mandate)
// ===========================================================================
//
// DateTime  (stdlib §2.5):
//   The instance call arm (rcn == "DateTime") only types Add*/Subtract/ToString.
//   DateTime is a `record` with compiler-synthesized ToString(), Equals(),
//   GetHashCode(), CompareTo() — none of these are handled in the call arm.
//   Adding a blanket else-diagnostic would incorrectly reject all synthesized
//   record methods.  Skip until a separate lane adds complete record-method
//   coverage to the DateTime arm.
//
// TimeSpan  (stdlib §2.5):
//   Same reasoning as DateTime.  The instance call arm only types
//   Add/Subtract/Negate; synthesized record methods fall through silently
//   and must continue to do so.  Skip for the same reason.
//
// IGrouping<TKey, T>  (stdlib §5):
//   IGrouping implements IEnumerable<T>.  LINQ extension methods (Where,
//   Select, ToArray, etc.) on an IGrouping receiver are routed through the
//   IGrouping call arm (rcn == "IGrouping"), NOT through the isLazyRecv
//   LINQ path (which requires no astNode).  A blanket else-diagnostic on the
//   IGrouping arm would reject all LINQ chaining on a GroupBy result.
//   Skip until the IGrouping arm handles the full IEnumerable<T> extension
//   surface or routes through the lazy-recv path.
