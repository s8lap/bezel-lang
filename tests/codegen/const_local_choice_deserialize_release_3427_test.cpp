// Codegen — issue #3427 hole 2 (§21.1 runtime `const` locals, §9.8
// "Serialization synthesis", §35.4 "The classification", §28.9 runtime-managed
// string reference counting, spec v0.286):
// `Emitter::collectValueLikeChoiceCallResultReleaseEligible` scanned a body's
// statements with `dynamic_cast<const ast::LocalDecl*>(s)`, so a §21.1 runtime
// `const` bound to a call on a value-like choice's own SYNTHESIZED
// `Deserialize` factory was never admitted as a candidate — the choice's
// payload string releases that #2428 arms for the `var` spelling never armed
// for the `const` one.
//
// ⚠ TWO INDEPENDENT MEASUREMENTS OF THIS COLLECTOR DISAGREED in the issue
// thread (see #3427's second comment): one repro used a `Msg.Deserialize(d)`
// initializer (the shape that reaches `isSynthesizedValueLikeChoiceDeserializeCall`)
// and measured a genuine `const`-only leak; another used a `C.Make(i)`
// initializer (an ordinary factory method, NOT the synthesized `Deserialize`)
// and measured identical leaks on both spellings — because that shape never
// reaches this collector's gate at all (#2115/#1747 territory, unrelated).
// RE-DERIVED on this lane's HEAD rather than inheriting either number: the
// `Msg.Deserialize(d)` shape below is confirmed `const`-only via
// `scripts/agent_check.sh --probe --asan`, matching measurement A exactly.
//
// FIX. `collectValueLikeChoiceCallResultReleaseEligible`'s candidate vector is
// widened from `const ast::LocalDecl*` to `const ast::Node*` and its scan reads
// the initializer/isUsing through `Emitter::localDeclInitView` — the single
// authority #3402 hoisted into LlvmBackend_impl.hpp — instead of the
// LocalDecl-only dynamic_cast. The consumer
// (`Emitter::lowerLocalDecl`, LlvmBackend_Setup.cpp ~14307) already keys
// `valueLikeChoiceCallResultReleaseEligible_` by raw node pointer and receives
// its `declNode`/`init`/`declType` triple generically for either decl kind
// (`lowerStmt`'s `LocalConstDecl` arm already calls the same `lowerLocalDecl`
// as the `LocalDecl` arm), so no consumer changes.
//
// §6b EXCLUSION DIFF. The pre-fix scan excluded EVERY §21.1 `const` bound to a
// synthesized-Deserialize call, unconditionally. `localDeclInitView` recognizes
// ONLY `ast::LocalDecl` and `ast::LocalConstDecl` (an all-null view for any
// other node — identical to the pre-fix behavior for every non-LocalDecl node,
// where the old dynamic_cast was also null), so the widened scan admits EXACTLY
// the §21.1 `const` case beyond the unchanged `var` behavior. The rest of the
// candidacy test (the call must resolve to the type's own SYNTHESIZED
// `Deserialize` factory, the resolved type must be the SAME non-nullable
// non-generic choice with a string reachable from its active case) is
// unchanged, so a hand-written `Deserialize` (which suppresses synthesis, §9.8)
// stays excluded for `const` exactly as it does for `var`.
//
// ⚠ ORACLE NOTE: every leak row below is a 200-iteration loop — a one-shot LSan
// run can report clean on BOTH spellings because the single allocation is
// still stack-reachable at exit (the same caveat #3427's own filed
// measurements record).
//
// FAIL-WITHOUT / PASS-WITH, measured via `scripts/agent_check.sh --probe --asan`
// on one build of each compiler (only this fix's LlvmBackend_Setup.cpp hunk
// reverted), 200 iterations of `const v := Msg.Deserialize(d);` (declared,
// never subsequently read — the simplest shape that reaches the collector's
// gate and is trivially proven non-escaping):
//
//   program                                          pre-fix            post-fix
//   const Msg.Deserialize(d) result, unused, x200    22400 B / 800      clean
//   var   Msg.Deserialize(d) result, unused, x200    clean              clean
//
// per §21.1, §9.8, §35.4, §28.9 (issue #3427)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
const char* kImport = "import System; import System.Serialization; ";

std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── THE RE-DERIVED REPRO, loop-amplified: `const v := Msg.Deserialize(d);`
// declared and never subsequently read, 200 times. Each iteration's
// deserialize call mints fresh payload strings via `bz_binder_read_bz_string`;
// the conservative escape walk proves the unused `const` non-escaping, so both
// mechanisms (isSynthesizedValueLikeChoiceDeserializeCall +
// the shared #2425 release walker) should arm — pre-fix they did not for a
// `const`. // per §21.1, §9.8, §35.4 (issue #3427)
BEZEL_TEST(const_local_choice_deserialize_release_3427,
           const_deserialize_call_result_released_spec_21_1_9_8_35_4) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "    using var s := new BinarySerializer(); "
        "    m.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    const v := Msg.Deserialize(d); "
        "    acc := acc + 1; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(acc.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "200\n");
}

// ── PARITY CONTROL: the `var` spelling of the identical program was clean
// pre-fix (#2428's own mechanism already worked for it) and must stay clean
// post-fix. // per §9.8, §35.4 (issue #3427)
BEZEL_TEST(const_local_choice_deserialize_release_3427,
           var_deserialize_call_result_released_parity_spec_9_8) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    var m := Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "    using var s := new BinarySerializer(); "
        "    m.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var v := Msg.Deserialize(d); "
        "    acc := acc + 1; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(acc.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "200\n");
}

// ── SAFETY GUARD (clean before AND after, single-shot — a structural exclusion
// pin, not a leak stress case): a HAND-WRITTEN `Deserialize` (suppresses
// synthesis, §9.8) bound to a `const` must NOT be admitted by
// `isSynthesizedValueLikeChoiceDeserializeCall` — its body is not provably a
// fresh construction in general, mirroring the `var` exclusion #2428 already
// established. This one happens to construct-and-return a fresh,
// literal-sentinel-payload value, so even a wrongly-armed release would be a
// safe no-op here; the pin is that the exclusion path still compiles and runs
// cleanly for the `const` spelling. // per §21.1, §9.8 (issue #3427)
BEZEL_TEST(const_local_choice_deserialize_release_3427,
           const_hand_written_deserialize_excluded_no_uaf_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Msg: ISerializable, IDeserializable "
        "  Text(first: string, second: string), Empty; "
        "  public static method Deserialize(d: IDeserializer): Msg "
        "    return Msg.Text(\"a-fairly-long-tag-value-string\", \"second-value-str\"); "
        "  end Deserialize; "
        "end Msg; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  const v := Msg.Deserialize(d); "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
