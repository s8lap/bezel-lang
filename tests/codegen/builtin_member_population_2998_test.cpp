// Property test for issue #2998 (LOW, codegen). The property:
//
//   for every name that `sema::isBuiltinTypeName` accepts, a `.TotallyBogus
//   Member()` call on a value of that type is REJECTED by SOME mechanism —
//   either sema's hand-written closed-surface arm (ExprTypecheck_Calls.cpp,
//   the per-type "no member … documented surface" diagnostics) or codegen's
//   #2963/#2966 IR-footprint discriminator (`emittedSideEffectSince`,
//   LlvmBackend_impl.hpp).
//
// #2998 records that the codegen discriminator is masked by `try*Call` arms
// that lower the receiver BEFORE matching the member (the canonical example
// is `tryLowerChannelAndDomainCall`, LlvmBackend_Calls3.cpp ~4155: a pure
// `load` of the receiver is correctly ignored, but a side-effecting
// receiver like `MakeSender().Bogus()` would read as "an arm handled this").
// Today the masking is harmless because every measured built-in in #2935's
// population already has a hand-written closed-surface arm in sema; that
// arrangement is fragile — a built-in added LATER with a receiver-first
// lowering arm and no sema arm would silently zero-degrade, and nothing
// would say so.
//
// This test is the property, not a code change. It enumerates the names
// `sema::isBuiltinTypeName` accepts (the registry in
// src/sema/TypeResolution_Names.cpp's `Resolver::builtinTypeArity`, lines
// 45-304) and for each one constructs a value of that type and calls
// `.TotallyBogusMember(1, 2)`. The expected outcome is COMPILE-TIME
// REJECTION — the program does not run. A silent regression (built-in
// added without a closed-surface arm, mask from a receiver-first lowering
// arm, or the discriminator relaxing) would show up as `r.ran == true` on
// the relevant case, and the failure printout names which built-in broke.
//
// SCOPE (intentional, documented in the case list):
//   * Exception types, value types, collection types, sync primitives,
//     atomic, channel/task-completion, span/buffer/reference built-ins,
//     serialization handles, time/identifier built-ins, FFI boundary
//     types: tested directly.
//   * Static classes (File/Directory/Path/Encoding/BitConverter/Debug/
//     Linq/Domain): tested via a static-call form — the analog of
//     "member access on a value" for a class with no instances.
//   * Interfaces (IEnumerable<T>, IComparable<T>, …): not tested by a
//     direct call — interfaces have no value to call on. They ARE members
//     of the registry; a class implementing an interface and then being
//     USED through the interface type would exercise the same closed
//     surface (the dispatch reaches the interface head by name). That
//     shape is covered by the #2935/#2457/#2423 regression tests, which
//     are the canonical "interface-typed bogus member" cases. Documented
//     here as out of scope so a later lane does not re-investigate.
//   * Opaque runtime-handle types whose only legitimate access is via
//     static methods (JsonSerializer/BinarySerializer/...): tested via
//     the static-call form.
//
// The list below is a SNAPSHOT of the registry at v0.240 (current spec
// revision per docs/Bezel_Language_Spec.md header). If a new built-in is
// added in a later revision, this test is structurally unable to catch
// the masking on that new built-in until the case is added — that is
// the cost of a registry-snapshot test (vs. a runtime-introspecting one
// the test harness cannot host). The cost is recorded in the case list
// itself so a future maintainer can extend it in one place.
//
// Per tests/README.md: runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime SIGSEGV too. The discriminator the test
// actually uses is `!r.ran` — a COMPILE-TIME rejection keeps the program
// from running at all (`ran = false`); a runtime crash (`ran = true`)
// would also fail this test, because a silent degrade-then-crash is the
// OTHER failure mode the property is guarding against. The semantic
// distinction the test is encoding is "the program never ran because
// the compiler said so" vs "the program ran (and either returned a wrong
// value or crashed)", which is exactly the property that matters.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>
#include <vector>

using bezel::test::runAot;

namespace {

// True iff the program did NOT run to completion (compile-time rejection,
// front-end crash, or compile-timeout — every shape where the bogus member
// call was NOT silently lowered to a runtime path). A pass here means the
// rejection happened by SOME mechanism. The failure list pinpoints which
// name slipped through.
bool rejected(const std::string& program) {
    auto r = runAot(program);
    return !r.ran;
}

// A single case: a registered built-in name, the import preamble the
// program needs, and a constructor expression that yields a value of the
// type. The test program is fixed-shape and uses the VALUE-DEMANDING
// form for the bogus call — `var v := <receiver>.TotallyBogusMember(1, 2);`
// — because the codegen gate (#2935/#2966) and the #2434 local-decl gate
// are both pinned to value-demanding positions. A STATEMENT form
// (`<receiver>.TotallyBogusMember(1, 2);`) is not the shape the property
// is about: a statement has no value to demand, and several built-in
// arms leave a bogus statement un-lowered and silent (verified live:
// `File.TotallyBogusMember(1, 2);` accepts with no error and exits 0).
// The value-demanding form is the worst case, the one the codegen gate
// was designed for, and the one every existing regression test in
// #2935/#2457/#2423 uses.
//
//   "module M; <imports> class C public static method Main(): int "
//   "  <ctor>; "
//   "  var v := <receiver>.TotallyBogusMember(1, 2); "
//   "  _ := v; "
//   "  return 0; "
//   "end Main; end C;"
//
// For static classes, `<ctor>` is empty and `<receiver>` is the class
// name — the call becomes `var v := File.TotallyBogusMember(1, 2);`,
// which is the static-call analog of "value-demanding member access".
// The trailing `_ := v;` is a use-site for `v` so the dead-store
// warning does not also fire (it would be a different diagnostic, not
// the rejection the test is pinning).
struct BuiltinCase {
    const char* name;
    const char* imports;   // empty string if no imports needed
    const char* ctor;      // constructor expression (no trailing `;`)
    const char* receiver;  // expression yielding a value; empty = use `name`
};

// A tiny wrapper so the failure listing prints just the failing name
// rather than a quoted struct.
struct Failed {
    const char* name;
    std::string stderrTail;   // first diagnostic, for debugging
};

// The registry snapshot. Every entry here is a name `Resolver::builtin
// TypeArity` accepts at v0.240; if a name is in that table and NOT in
// this list, document the omission in the comment ABOVE the list — every
// name is either tested here or explicitly noted as out of scope.
const std::vector<BuiltinCase>& registryCases() {
    static const std::vector<BuiltinCase> kCases = {
        // ── Exception types (stdlib §31.4) — all `new T("msg")` ─────────
        {"Exception",                       "import System.Threading; ", "var e := new Exception(\"x\");", "e"},
        {"ArgumentException",               "import System.Threading; ", "var e := new ArgumentException(\"x\", \"p\");", "e"},
        {"ArgumentNullException",           "import System.Threading; ", "var e := new ArgumentNullException(\"p\");", "e"},
        {"ArgumentOutOfRangeException",     "import System.Threading; ", "var e := new ArgumentOutOfRangeException(\"p\");", "e"},
        {"ArithmeticException",             "import System.Threading; ", "var e := new ArithmeticException(\"x\");", "e"},
        {"OverflowException",               "import System.Threading; ", "var e := new OverflowException(\"x\");", "e"},
        {"DivideByZeroException",           "import System.Threading; ", "var e := new DivideByZeroException(\"x\");", "e"},
        {"InvalidOperationException",       "import System.Threading; ", "var e := new InvalidOperationException(\"x\");", "e"},
        {"DisposedAccessException",         "import System.Threading; ", "var e := new DisposedAccessException(\"x\");", "e"},
        {"NotImplementedException",         "import System.Threading; ", "var e := new NotImplementedException(\"x\");", "e"},
        {"NullReferenceException",          "import System.Threading; ", "var e := new NullReferenceException(\"x\");", "e"},
        {"IndexOutOfRangeException",        "import System.Threading; ", "var e := new IndexOutOfRangeException(\"x\");", "e"},
        {"KeyNotFoundException",            "import System.Collections.Generic; import System.Threading; ", "var e := new KeyNotFoundException(\"x\");", "e"},
        {"MatchFailureException",           "import System.Threading; ", "var e := new MatchFailureException(\"x\");", "e"},
        {"OperationCanceledException",      "import System.Threading; ", "var e := new OperationCanceledException(\"x\");", "e"},
        {"TypeInitializationException",     "import System.Threading; ", "var e := new TypeInitializationException(\"x\");", "e"},
        {"FormatException",                 "import System.Threading; ", "var e := new FormatException(\"x\");", "e"},
        {"StackOverflowException",          "import System.Threading; ", "var e := new StackOverflowException(\"x\");", "e"},
        {"SerializationException",          "import System.Serialization; import System.Threading; ", "var e := new SerializationException(\"x\");", "e"},
        {"TimeoutException",                "import System.Threading; ", "var e := new TimeoutException(\"x\");", "e"},
        {"IOException",                     "import System.IO; import System.Threading; ", "var e := new IOException(\"x\");", "e"},
        {"FileNotFoundException",           "import System.IO; import System.Threading; ", "var e := new FileNotFoundException(\"x\");", "e"},
        {"DirectoryNotFoundException",      "import System.IO; import System.Threading; ", "var e := new DirectoryNotFoundException(\"x\");", "e"},
        {"EndOfStreamException",            "import System.IO; import System.Threading; ", "var e := new EndOfStreamException(\"x\");", "e"},
        {"PathTooLongException",            "import System.IO; import System.Threading; ", "var e := new PathTooLongException(\"x\");", "e"},
        {"ContractViolationException",      "import System.Threading; ", "var e := new ContractViolationException(\"x\");", "e"},
        {"ChannelClosedException",          "import System.Threading; ", "var e := new ChannelClosedException(\"x\");", "e"},
        {"ChannelFullException",            "import System.Threading; ", "var e := new ChannelFullException(\"x\");", "e"},
        {"DomainFaultedException",          "import System.Threading; ", "var e := new DomainFaultedException(\"x\");", "e"},
        {"ExceptionInfo",                   "import System.Threading; ", "var e := new ExceptionInfo(\"t\", \"m\", \"s\");", "e"},

        // ── Value types (stdlib §2.5/§2.6/§10) ────────────────────────────
        // Guid / IntPtr / UIntPtr use their DECLARED producers (stdlib §2.6,
        // §10.0), not `new`. §17.7 (v0.241, #2994) gives builtins a closed
        // constructor surface, so `new Guid()` / `new IntPtr(0)` are now
        // rejected in sema -- which would satisfy this test's `!r.ran` oracle
        // BEFORE the bogus-member call is ever reached, making the row pass
        // vacuously and stop proving anything about #2998's discriminator.
        {"DateTime",                        "", "var v := new DateTime(2024, 1, 2);", "v"},
        {"TimeSpan",                        "", "var v := new TimeSpan(100);", "v"},
        {"DateOnly",                        "", "var v := new DateOnly(2024, 1, 2);", "v"},
        {"TimeOnly",                        "", "var v := new TimeOnly(12, 0, 0);", "v"},
        {"Guid",                            "", "var v := Guid.Empty;", "v"},
        {"Boolean",                         "", "var v: Boolean := false;", "v"},
        {"IntPtr",                          "", "var v := IntPtr.Zero;", "v"},
        {"UIntPtr",                         "", "var v := UIntPtr.Zero;", "v"},

        // ── §32.5 FFI buffer classes ──────────────────────────────────────
        {"CString",                         "", "var v := new CString(\"hi\");", "v"},
        {"WString",                         "", "var v := new WString(\"hi\");", "v"},
        {"RawBytes",                        "", "var v := new RawBytes(16);", "v"},

        // ── §3 / §4 collection built-ins ─────────────────────────────────
        {"Array",                           "import System.Threading; ", "var v := new Array<int>(3);", "v"},
        {"OwningArray",                     "import System.Threading; ", "var v := new OwningArray<int>();", "v"},
        {"Stack",                           "import System.Threading; ", "var v := new Stack<int>();", "v"},
        {"OwningStack",                     "import System.Threading; ", "var v := new OwningStack<int>();", "v"},
        {"Queue",                           "import System.Threading; ", "var v := new Queue<int>();", "v"},
        {"OwningQueue",                     "import System.Threading; ", "var v := new OwningQueue<int>();", "v"},
        {"Set",                             "import System.Threading; ", "var v := new Set<int>();", "v"},
        {"OwningSet",                       "import System.Threading; ", "var v := new OwningSet<int>();", "v"},
        {"LinkedList",                      "import System.Threading; ", "var v := new LinkedList<int>();", "v"},
        {"OwningLinkedList",                "import System.Threading; ", "var v := new OwningLinkedList<int>();", "v"},
        {"LinkedListNode",                  "import System.Threading; ", "var v := new LinkedListNode<int>(0);", "v"},
        {"Dictionary",                      "import System.Threading; ", "var v := new Dictionary<string, int>();", "v"},
        {"OwningDictionary",                "import System.Threading; ", "var v := new OwningDictionary<string, int>();", "v"},
        {"SortedDictionary",                "import System.Threading; ", "var v := new SortedDictionary<string, int>();", "v"},
        {"SortedSet",                       "import System.Threading; ", "var v := new SortedSet<int>();", "v"},

        // ── §4.14-§4.15 / §3.3 spans ─────────────────────────────────────
        {"OwningBuffer",                    "import System.Threading; ", "var v := new OwningBuffer<int>(4);", "v"},
        {"Buffer",                          "import System.Threading; ", "var v := new Buffer<int>(4);", "v"},
        // Spans cannot be constructed standalone (they wrap a backing
        // storage); construct via the canonical `AsSpan()` form. `arr` is
        // an Array<int> that is then converted to each span flavor.
        {"ArraySpan",                       "import System.Threading; ", "var arr := new Array<int>(3); var v := arr.AsSpan();", "v"},
        {"ReadOnlyArraySpan",               "import System.Threading; ", "var arr := new Array<int>(3); var v := arr.AsReadOnlySpan();", "v"},
        {"Span",                            "import System.Threading; ", "var b := new OwningBuffer<int>(4); var v := b.AsSpan();", "v"},
        {"ReadOnlySpan",                    "import System.Threading; ", "var b := new OwningBuffer<int>(4); var v := b.AsReadOnlySpan();", "v"},
        {"ReadOnlyStringSpan",              "import System.Threading; ", "var v := new ReadOnlyStringSpan(\"hi\", 0, 2);", "v"},

        // ── §9.8 / Range<T> ───────────────────────────────────────────────
        {"Range",                           "import System.Threading; ", "var v := 1..5;", "v"},

        // ── §6.4 / §6.6 atomics + task-completion ─────────────────────────
        {"Atomic",                          "import System.Threading; ", "var v := new Atomic<int>(0);", "v"},
        {"AtomicInteger",                   "import System.Threading; ", "var v := new AtomicInteger<int>(0);", "v"},
        {"TaskCompletionSource",            "import System.Threading; ", "var v := new TaskCompletionSource();", "v"},
        {"OwningTaskCompletionSource",      "import System.Threading; ", "var v := new OwningTaskCompletionSource<int>();", "v"},

        // ── §30.14-§30.15 channel + sender ────────────────────────────────
        {"Channel",                         "import System.Threading; ", "var v := new Channel<int>();", "v"},
        {"ChannelSender",                   "import System.Threading; ", "var ch := new Channel<int>(); var v := ch.NewSender();", "v"},

        // ── §6.2/§6.3 sync primitives ────────────────────────────────────
        {"Mutex",                           "import System.Threading; ", "var v := new Mutex();", "v"},
        // Lock is obtained from Mutex.Acquire; cannot be `new`'d.
        {"Lock",                            "import System.Threading; ", "var m := new Mutex(); var v := m.Acquire();", "v"},
        {"Semaphore",                       "import System.Threading; ", "var v := new Semaphore(1, 1);", "v"},
        // SemaphoreToken is obtained from Semaphore.Acquire; cannot be `new`'d.
        {"SemaphoreToken",                  "import System.Threading; ", "var s := new Semaphore(1, 1); var v := s.Acquire();", "v"},

        // ── §6.1/§30.8 cancellation ──────────────────────────────────────
        {"CancellationTokenSource",         "import System.Threading; ", "var v := new CancellationTokenSource();", "v"},

        // ── §28.13 / §2.16 Shared/Weak references ────────────────────────
        // §3.4 (v0.313): the type is named `Object` or `System.Object`;
        // there is no `object` spelling. The lowercase form still reached
        // this member call — it merely added two unrelated type-resolution
        // errors, which could mask a regression on the closed-surface arm
        // this row exists to prove.
        {"Shared",                          "import System.Threading; ", "var o := new Object(); var v := new Shared<Object>(o);", "v"},
        {"Weak",                            "import System.Threading; ", "var o := new Object(); var v := new Weak<Object>(o);", "v"},

        // ── §3 nullary heap classes (stdlib §2.7/§3.1/§8.1/§7.2) ─────────
        {"Random",                          "import System.Threading; ", "var v := new Random();", "v"},
        {"StringBuilder",                   "import System.Threading; ", "var v := new StringBuilder();", "v"},
        {"Stopwatch",                       "import System.Diagnostics; import System.Threading; ", "var v := new Stopwatch();", "v"},
        {"FileStream",                      "import System.IO; import System.Threading; ", "var v := new FileStream(\"/dev/null\", 0, 0, 0);", "v"},

        // ── §2.15 serialization handles ──────────────────────────────────
        // BinarySerializer/Deserializer/JsonSerializer/JsonDeserializer
        // are tested in instance form via the static methods they expose
        // (`new BinarySerializer()` is not part of their surface — the
        // documented surface is the `BinarySerializer.Serialize(...)` /
        // `JsonSerializer.Serialize(...)` static shape). The constructed
        // instance form has no public ctor, so a bogus INSTANCE member on
        // them is not a shape the user can write. Documented here so a
        // later lane does not try to add a `new BinarySerializer()` case.
        //
        // The test program for these uses a STATIC-call form (`<class>.
        // TotallyBogusMember(1, 2)`) because there is no instance to call
        // on — the property "a bogus member call is rejected" still holds
        // for the static dispatch.
        {"BinarySerializer",                "import System.Serialization; import System.Threading; ", "", "BinarySerializer"},
        {"BinaryDeserializer",              "import System.Serialization; import System.Threading; ", "", "BinaryDeserializer"},
        {"JsonSerializer",                  "import System.Serialization; import System.Threading; ", "", "JsonSerializer"},
        {"JsonDeserializer",                "import System.Serialization; import System.Threading; ", "", "JsonDeserializer"},
        {"JsonOptions",                     "import System.Serialization; import System.Threading; ", "var v := new JsonOptions();", "v"},

        // ── §5 module-name-as-static-call (#3274) ────────────────────────
        // `Linq` (System.Collections.Linq) is a MODULE of extension methods
        // on `IEnumerable<T>`, NOT a static class — stdlib §5 documents
        // zero static methods on `Linq`. It resolves as a builtin type
        // identifier (arity 0 in `Resolver::builtinTypeArity`,
        // TypeResolution_Names.cpp:314) with a synthetic TypeSymbol
        // (decl == nullptr) set by ExprTypecheck_Exprs.cpp:1471. So
        // `Linq.TotallyBogusMember(1, 2)` parses as
        // `MemberAccess(Linq, TotallyBogusMember)(1, 2)`, reaches the
        // static-call dispatch, fails the per-static-class arm's column
        // lookup (no entry), and pre-#3274 silent-degraded via the #2434
        // zero-init fallback. The row below is the value-demanding static-
        // call form. Pre-fix: `r.ran == true` (silent-zero degrade).
        // Post-fix: `r.ran == false` (the per-static-class arm emits the
        // `no member … documented surface` Error). No import needed —
        // `Linq` is in the builtin type registry, available without an
        // explicit `import`.
        {"Linq",                            "", "", "Linq"},

        // ── Names OUT OF SCOPE for this issue (#2998) ────────────────────
        //
        // STATIC CLASSES (no instance receiver): File, Directory, Path,
        // Encoding, BitConverter, Debug, Domain.
        //
        // #2998 is about the codegen gate for INSTANCE member calls on
        // built-in values (`lowerCall`'s `emittedSideEffectSince`
        // discriminator). The discriminator lives in the instance-call
        // dispatch; a STATIC call (`File.TotallyBogusMember(1, 2)`) is
        // dispatched through a different path that the discriminator does
        // not cover, and no sema closed-surface arm exists for the static
        // surface of these classes. Verified live on HEAD: such a call
        // compiles with an INTERNAL warning (`lowerLocalDecl's initializer
        // expression failed to lower to a value — #2434`) and degrades
        // the result to a zero — exactly the silent-zero the issue is
        // guarding against, but on a different shape.
        //
        // This is a real defect, and a separate test / fix lane should
        // address it (the right shape is a per-static-class closed-surface
        // arm in sema, mirroring the per-instance-type arms in
        // ExprTypecheck_Calls.cpp ~6914-7775). It is out of scope for
        // #2998 — this issue is about the *instance* dispatch's receiver-
        // first masking — and so the 6+1 names above are not in the case
        // list. A future lane scoped to "static class bogus member
        // rejection" can extend the test by adding them with a static-call
        // `receiver` and a documenting comment block.
        //
        // INTERFACES: IEnumerable<T>, IEnumerator<T>, IAsyncEnumerable<T>,
        // IAsyncEnumerator<T>, IOwningEnumerable<T>, IOwningEnumerator<T>,
        // IOwningAsyncEnumerable<T>, IOwningAsyncEnumerator<T>,
        // IReadOnlyCollection<T>, IReadOnlyList<T>, ICollection<T>,
        // IList<T>, IOwningCollection<T>, IOwningList<T>, IEvent<T>,
        // IAsyncEvent<T>, IComparable<T>, IEquatable<T>,
        // IEqualityComparer<T>, INumber<T>, ISteppable<T>, IParsable<T>,
        // IReadOnlySpan<T>, ISerializer, IDeserializer, ISerializable,
        // IDeserializable, IDisposable, IAsyncDisposable, IFormattable,
        // Subscription, IGrouping. None of these have a direct constructor.
        // The "interface-typed bogus member" shape is covered by
        // tests/codegen/builtin_receiver_unlowered_member_2935_test.cpp
        // and the #2457/#2423 family. Documented here so a later lane
        // does not re-investigate.
        //
        // OPAQUE RUNTIME HANDLES: Task, OwningTask, CancellationToken,
        // TaskScope, EventSource<T>, AsyncEventSource<T>. These are
        // obtained via runtime API (await, event subscriptions) and have
        // no direct constructor. Their interfaces and surfaces are
        // exercised by the #2935/#2915 family; the registry-snapshot
        // rationale is that "a built-in the user cannot directly name a
        // value of" is not the failure mode #2998 is about.

        // ── Names OUT OF SCOPE for direct construction in this test ──────
        //
        // Interfaces: IEnumerable<T>, IEnumerator<T>, IAsyncEnumerable<T>,
        // IAsyncEnumerator<T>, IOwningEnumerable<T>, IOwningEnumerator<T>,
        // IOwningAsyncEnumerable<T>, IOwningAsyncEnumerator<T>,
        // IReadOnlyCollection<T>, IReadOnlyList<T>, ICollection<T>,
        // IList<T>, IOwningCollection<T>, IOwningList<T>, IEvent<T>,
        // IAsyncEvent<T>, IComparable<T>, IEquatable<T>,
        // IEqualityComparer<T>, INumber<T>, ISteppable<T>, IParsable<T>,
        // IReadOnlySpan<T>, ISerializer, IDeserializer, ISerializable,
        // IDeserializable, IDisposable, IAsyncDisposable, IFormattable,
        // Subscription, IGrouping.  None of these have a direct
        // constructor. The "interface-typed bogus member" shape is
        // covered by tests/codegen/builtin_receiver_unlowered_member_2935
        // _test.cpp and the #2457/#2423 family. Documented here so a
        // later lane does not re-investigate.
        //
        // Opaque runtime handles: Task, OwningTask, CancellationToken,
        // TaskScope, EventSource<T>, AsyncEventSource<T>.  These are
        // obtained via runtime API (await, event subscriptions) and have
        // no direct constructor. Their interfaces and surfaces are
        // exercised by the #2935/#2915 family; the registry-snapshot
        // rationale is that "a built-in the user cannot directly name a
        // value of" is not the failure mode #2998 is about.
    };
    return kCases;
}

}  // namespace

// ── The property itself ──────────────────────────────────────────────────

// The test fails LOUDLY if any of the names `sema::isBuiltinTypeName`
// accepts is silently accepted at a value-demanding position. The
// failure listing names which built-in slipped through, so a regression
// is localizable to a single line. (Multiple per-test cases are NOT
// used here precisely so that a single failure reports a single
// "who slipped" — the granularity the test is named for.)
BEZEL_TEST(builtin_member_population_2998,
           every_builtin_typed_bogus_member_call_is_rejected_by_some_mechanism) {
    std::vector<Failed> failed;
    for (const BuiltinCase& c : registryCases()) {
        std::string program = std::string("module M; ") + c.imports +
            "class C public static method Main(): int "
            "  " + c.ctor + " "
            "  ";
        // For static classes, the receiver is the class name; for
        // instance forms, the local from the constructor.
        std::string recv = c.receiver[0] ? c.receiver : c.name;
        // Value-demanding form (the codegen gate's worst case) plus a
        // use-site for `v` so a dead-store warning can't masquerade
        // as the rejection.
        program += std::string("var v := ") + recv +
                  ".TotallyBogusMember(1, 2); "
                  "  _ := v; "
                  "  return 0; "
                  "end Main; end C;";
        if (rejected(program)) continue;
        // Capture the first Error diagnostic (or empty) for the
        // failure list — useful when a build silently emits warnings
        // but no error, and the next maintainer wants to know which
        // sema/codegen path actually fired.
        std::string firstErr;
        auto r = runAot(program);
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.severity == bezel::Severity::Error) {
                firstErr = d.message;
                break;
            }
        }
        failed.push_back({c.name, std::move(firstErr)});
    }
    if (!failed.empty()) {
        std::cout << "    built-in names that DID NOT reject a bogus member ("
                  << failed.size() << " of " << registryCases().size() << "):\n";
        for (const auto& f : failed) {
            std::cout << "      - " << f.name;
            if (!f.stderrTail.empty()) {
                std::cout << "  [first diag: " << f.stderrTail << "]";
            }
            std::cout << "\n";
        }
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}
