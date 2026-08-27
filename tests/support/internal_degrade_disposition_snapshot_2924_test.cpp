// A SNAPSHOT of every `internal:` codegen degrade that SHIPS A BINARY, so that
// a thirteenth one is a loud test failure with an actionable message rather
// than a silent new #2922.
//
// WHY THIS EXISTS (#2924).  Three separate issues — #2434, #2491, #2775 — each
// took one silent codegen degrade and made it OBSERVABLE, by putting a
// `Severity::Warning` next to the zero/null substitution. All three were right
// on their own terms, and the Warning severity is DELIBERATE and documented:
// LlvmBackend_Setup.cpp says so outright — "Warning, not Error, per §3/#605's
// permissive shapes; !hasErrors() guard" — because #2434's own empirical
// lit-corpus check found six currently-legal programs reaching the degrade.
// This test does not propose flipping any of them.
//
// What was missing is the RECORD.  A `Severity::Error` degrade fails the build
// and is safe whatever it substitutes.  A `Severity::Warning` degrade compiles,
// ships a binary, and nothing said, per site, whether that binary is merely
// WRONG or is MEMORY-UNSAFE.  Both outcomes have now been measured from the
// SAME reporter:
//
//   * #2922 — a degraded `ToString()` leaves a NULL `string` in the slot, and
//     `s.Length` on it reads absolute address 0x8 (ASan: "SEGV on unknown
//     address 0x000000000008 … Hint: address points to the zero page").
//   * the same reporter on a `bool`-typed local emits an `i32` where the
//     language type is `bool` — caught only because a downstream `br` demanded
//     an `i1` ("Branch condition is not 'i1' type!").  Bind it and never branch
//     and the wrongly-typed value ships.
//
// So the distinction is real rather than hypothetical, and three of one run's
// HIGH filings (#2921, #2922, #2923) are downstream of it never being asked.
//
// WHAT THIS TEST DOES.  It scans `src/` for every `"internal: ` diagnostic
// string, resolves each one's `Severity`, and asserts that the set of
// WARNING-severity sites is exactly the snapshot below — where each entry
// carries a DISPOSITION (`WrongValue` / `UnsafePointer`) and a one-line reason.
// It asserts nothing about whether a disposition is *right*; like
// tests/module/semacontext_ast_side_table_snapshot_test.cpp (which closed a
// five-bug family the same way), its whole value is that someone has to DECIDE,
// once, and record the decision where the next reader will find it.
//
// ⚠ IF THIS TEST FAILS because you added a `Severity::Warning` degrade: do not
// just add the entry.  Decide first whether the zero/null you are substituting
// can land in a POINTER-typed slot that something later dereferences.  If it
// can, that is a memory-safety hazard shipping inside a successful compile —
// make it an Error for the shapes where a value is DEMANDED (see #2922's fix at
// the §8.1 member gate, and #2935's at the local-declaration consumer), or say
// here why it cannot be.
//
// WHY ONLY THE WARNING SITES ARE PINNED BY NAME.  The ~38 `Severity::Error`
// `internal:` sites are ICE guards: they fail the build, so they are safe
// whatever they substitute, and new ones get added routinely as new lowering
// paths acquire defensive checks.  Pinning them by name would churn on every
// such addition, and a churny snapshot gets its number bumped without thought —
// which is the failure mode this test exists to prevent.  #2924's question is
// about the sites that do NOT fail the build, and that is the set pinned here.
// The scan does still require every `"internal: ` string it finds to resolve to
// a definite severity, so a site the scanner cannot classify fails loudly
// instead of being silently omitted from the population.
//
// KEYING.  Each entry is keyed on {file, issue tag} — every one of these
// messages ends by naming the issue it came from — and NOT on a line number,
// which would churn on every unrelated edit above it, nor on a prose prefix,
// which several of these build by concatenating a runtime value into the middle
// of the string.
//
// NOT DONE HERE, still open under #2924: the #2491 diagnostic exists as FOUR
// hand-written copies (the four `#2491` entries below), three of which point at
// the fourth for their rationale.  A fifth return path added later gets the
// silent degrade with no diagnostic at all, and nothing would catch that.
// Collapsing them into one `reportDegradedReturn(span)` helper is a code change
// with its own risk and is deliberately not bundled into this test-infra
// commit; the snapshot at least makes the four visible as four.  (#2966 is the
// nearest related work: three of the four are also the sites #2935's fix could
// not safely extend to, and it would want the same shared helper.)

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "test_harness.hpp"

namespace {

// Can the zero/null this site substitutes reach a POINTER that something later
// dereferences, or is the degraded value confined to a non-pointer?
enum class Disposition { WrongValue, UnsafePointer };

struct Entry {
    const char* file;      // path relative to the repo root
    const char* issueTag;  // e.g. "#2491" — the tag the message itself carries
    Disposition disposition;
    const char* reason;
};

// The WARNING-severity `internal:` degrades (sixteen as of v0.240; #3079 added
// at v0.249; the two #3157 alarms and #3437's representation guard at v0.283),
// each with the
// judgement #2924 asks for.  Ordered by file, then by the tag.  The live count
// is `kN`, derived from this array — the prose above is a changelog, not the
// number the test asserts.
const Entry kWarningDegrades[] = {
    {"src/codegen/LlvmBackend_Assign.cpp", "#2833",
     Disposition::UnsafePointer,
     "a `this[i]`/`base[i]` indexer READ is dropped and 'degrades to a "
     "zero/null value' (the site's own words). On a class-typed indexer that "
     "zero IS the null pointer the caller then dereferences"},

    {"src/codegen/LlvmBackend_Assign.cpp", "#2491",
     Disposition::UnsafePointer,
     "property-getter face: substitutes `llvm::Constant::getNullValue(valTy)` "
     "straight into `CreateRet`. For a reference-typed property that is a null "
     "pointer returned to a caller sema promised a non-null object"},

    {"src/codegen/LlvmBackend_Calls2.cpp", "#2477",
     Disposition::UnsafePointer,
     "the member-dispatch fallthrough only NAMES the failure; the substitution "
     "is lowerLocalDecl's zero-init (#2434, below), which #2922 measured "
     "producing a null `string` and a SIGSEGV at address 0x8. The §8.1 "
     "universal-member subset of this fallthrough is now a located Error "
     "(#2922); what remains is the §3/#605 permissive surface"},

    {"src/codegen/LlvmBackend_Calls2.cpp", "#2491",
     Disposition::UnsafePointer,
     "generic-specialization expression-body face: same "
     "`emitReturn(fn, nullptr)` -> `getNullValue(retTy)` null-pointer return"},

    {"src/codegen/LlvmBackend_Exprs.cpp", "#2547",
     Disposition::UnsafePointer,
     "a member READ with no dispatch; the site's own text says CG8's "
     "fall-through 'degrades the slot to zero via lowerLocalDecl's "
     "initializer-failed fallback' — the #2434 null-pointer path"},

    {"src/codegen/LlvmBackend_Exprs2.cpp", "#2805",
     Disposition::WrongValue,
     "`degradedBoolOperand` returns `builder_.getInt1(false)`. The substituted "
     "value is an i1 by construction and can never be a pointer; the harm is "
     "bounded to a short-circuit operand reading as `false`"},

    {"src/codegen/LlvmBackend_Exprs2.cpp", "#2477",
     Disposition::WrongValue,
     "`new T()` in the DEAD abstract generic template body, which is never "
     "invoked — real calls route through the monomorphized clone — so no value "
     "it degrades is ever read at runtime"},

    {"src/codegen/LlvmBackend_Exprs2.cpp", "#2658",
     Disposition::UnsafePointer,
     "the imported constructor body never runs, so 'every field it assigns "
     "keeps its default' — a zeroed instance whose class-typed fields are null "
     "references the program will go on to read as objects"},

    {"src/codegen/LlvmBackend_Layout.cpp", "#2775",
     Disposition::WrongValue,
     "the dropped `is` test reads as `false`, an i1. Wrong answer, never a "
     "pointer — and the shapes where a non-answer is legitimate (dead template "
     "stub, erased operand) are excluded from the report"},

    {"src/codegen/LlvmBackend_Setup.cpp", "#2491",
     Disposition::UnsafePointer,
     "arrow-body (`=> expr`) face: same `emitReturn(fn, nullptr)` -> "
     "`getNullValue(retTy)` null-pointer return"},

    {"src/codegen/LlvmBackend_Setup.cpp", "#2434",
     Disposition::UnsafePointer,
     "the MEASURED one: #2922 reproduced a null `string` in the degraded slot "
     "and a `.Length` read faulting at address 0x8 (ASan: 'address points to "
     "the zero page'). Its REACH has since been narrowed twice without the "
     "site moving — #2915 exempts a UNIT-typed initializer (having no lowered "
     "value is that type's representation, not a failure), and #2922/#2935 "
     "turned the member-call shapes that DEMAND a value into Errors — but what "
     "it still substitutes when it fires is unchanged: a null pointer on the "
     "§3/#605 permissive surface"},

    {"src/codegen/LlvmBackend_Setup.cpp", "#2992",
     Disposition::WrongValue,
     "the end-of-chain guard emits the note and substitutes NOTHING — it falls "
     "through with the local's real value intact (registerUsingDisposal returns "
     "with no store altered). The harm it flags is a missing disposal arm, a "
     "bounded recoverable LEAK, never a zero/null read into a pointer slot; it "
     "is a compiler-bug alarm, not a value-substituting degrade"},

    {"src/codegen/LlvmBackend_Setup.cpp", "#3437",
     Disposition::WrongValue,
     "the `await using` representation guard in registerUsingDisposal: when "
     "`iterFormatWrappedDecls_` says the slot holds a §29.2 iterator-format "
     "wrapper frame, the generic async-disposal fallback's premise (word 0 is "
     "a CLASS vtable) is known false, so the site reports and registers "
     "NOTHING. Same leak-not-substitute shape as the #2992 twin above and the "
     "two #3157 alarms: no zero/null is manufactured and the slot keeps the "
     "frame it already held; the bounded harm is one un-reclaimed frame at "
     "scope end. It is the deliberate replacement for what the fallback did "
     "instead — read the frame's word 0 as a vtable and call slot 2, which in "
     "the iterator format is MoveNext (#2975's wild-jump shape) — so this "
     "Warning stands where a memory-safety fault used to ship silently. "
     "Unreachable as of #3437's first hunk (the iterator-format arm now claims "
     "every such decl before this point); it is a ratchet against the next "
     "edit re-opening the hole, tracked structurally as #3419"},

    {"src/codegen/LlvmBackend_Stmts.cpp", "#2491",
     Disposition::UnsafePointer,
     "the ReturnStmt face, which carries the family's full rationale: same "
     "`emitReturn(fn, nullptr)` -> `getNullValue(retTy)` null-pointer return. "
     "Narrowed like its #2434 twin (a unit-typed return value is #2915's "
     "exemption; a built-in member call that demanded a value is #2935's "
     "Error) without changing what it substitutes when it does fire"},

    {"src/codegen/LlvmBackend_Stmts.cpp", "#2961",
     Disposition::WrongValue,
     "the AssignmentStmt localSlot arm: `if (!rhs) return false;` keeps the "
     "target's PREVIOUS value rather than substituting `getNullValue`. A wrong "
     "value (the slot keeps stale state) is bounded to the assignment's "
     "target — no null pointer escapes, no dereference site is fed garbage. "
     "Mirror of #2961's twin below at the generic lvalueAddr arm."},

    {"src/codegen/LlvmBackend_Stmts.cpp", "#2961",
     Disposition::WrongValue,
     "the AssignmentStmt generic lvalueAddr arm (the `this.f := <expr>` and "
     "indexer-set cases): same `if (!rhs) return false;` keep-previous-value "
     "shape as the localSlot arm above. The retained value is whatever was "
     "stored there before; no null-pointer substitution, no UAF. The hazard "
     "is a stale field/indexer slot — bounded, observable, and now diagnosed."},

    {"src/codegen/LlvmBackend_Stmts.cpp", "#3025",
     Disposition::WrongValue,
     "the dedicated indexer-setter arm returns before invoking any setter or "
     "store when its RHS has no lowered value. The slot keeps its previous, "
     "already-valid value; the site neither manufactures nor substitutes a "
     "pointer. This is the dedicated-arm residual outside #2961."},

    {"src/codegen/LlvmBackend_Stmts.cpp", "#3025",
     Disposition::WrongValue,
     "the `??=` null-branch arm evaluates the RHS, then branches around the "
     "store when no value was lowered. The nullable target keeps its previous "
     "null value; nullable member access still requires narrowing or the "
     "checked `!` unwrap. No raw pointer is substituted — the bounded harm is "
     "that the coalescing assignment failed to establish a value."},

    {"src/codegen/LlvmBackend_Calls.cpp", "#3079",
     Disposition::WrongValue,
     "the third value-demanding degrade (#2966's named residual): a "
     "`Console.Write`/`WriteLine` whose single argument failed to lower. "
     "Branch (c) of the guard — the \u00a73/#605 permissive surface, which "
     "reaches this BY DESIGN — reports and leaves control flow unchanged, so "
     "the statement still emits nothing. NO value is substituted at all: the "
     "site returns `false` (\"not a console call\") rather than handing a "
     "zero or null onward, so unlike #2434/#2477 there is no pointer for a "
     "caller to dereference. The bounded harm is an ABSENT statement — the "
     "narrowest of the three dispositions in this table, and the reason this "
     "is a Warning while branches (a)/(b) escalate. Tracked for the residual "
     "sites (ordinary call arguments, the `?.` twin) as #3105."},

    {"src/codegen/LlvmBackend_Disposal.cpp", "#3157",
     Disposition::WrongValue,
     "the owned-capture arm (2) 'no release arm yet' alarm in "
     "emitOwnedParamCaptureDispose — the slot is LEFT UN-RECLAIMED rather "
     "than substituted, so no zero/null lands in a pointer slot. Mirror of "
     "#2992 above (same leak-not-crash shape): the alarm is a compiler-bug "
     "report whose harm is a bounded leak at frame teardown, never a "
     "dereference fed garbage. The previous default DID manufacture a value "
     "(the wild vtable jump into seed/opaque bytes) — that path is exactly "
     "what #3157's arm split removed, so this Warning is the replacement "
     "for the SEGV that used to ship silently."},

    {"src/codegen/LlvmBackend_Disposal.cpp", "#3157",
     Disposition::WrongValue,
     "the owned-capture arm (4) 'unrecognised type' alarm in "
     "emitOwnedParamCaptureDispose — same leak-not-substitute shape as the "
     "(2) alarm above and the #2992 twin: the slot is left un-reclaimed, no "
     "zero/null is manufactured, no pointer slot is fed garbage. The "
     "population this can fire on is now small by construction (#3157's "
     "arms (1)/(2)/(3) cover Task, every other vtable-less built-in, and "
     "every recognised class — local, imported, and built-in exception — "
     "so what reaches (4) is a primitive, a borrow-only record, or an alias "
     "into (3), none of which an `owned` capture can legitimately declare)."},
};

// How many of these can hand a user a memory-unsafe binary. Pinned as its own
// number so that changing a disposition is a deliberate edit in two places.
// The two #2961 and two #3025 entries are WrongValue (no null pointer is
// substituted), so this count was unchanged at 9 from the twelve-degrade
// population; #3079 (v0.249) is likewise WrongValue — it substitutes nothing
// at all, it declines the site — so the count stays 9.
constexpr std::size_t kUnsafePointerCount = 9;

// Locate the repo root from this translation unit's own path:
// <root>/tests/support/<this file>.
std::string repoRoot() {
    std::string p = __FILE__;
    for (int up = 0; up < 3; ++up) {
        std::size_t slash = p.find_last_of('/');
        if (slash == std::string::npos) return {};
        p.resize(slash);
    }
    return p;
}

struct Site {
    std::string file;      // relative to the repo root
    int line = 0;
    std::string severity;  // "Error" / "Warning"; empty when unresolvable
    std::string text;      // the whole report(...) call, from `"internal: `
};

bool isCommentLine(const std::string& line) {
    std::size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return true;
    return line.compare(i, 2, "//") == 0 || line.compare(i, 1, "*") == 0;
}

// Every `"internal: ` occurrence in `text` that is not inside a comment.
//
// The severity comes from the nearest PRECEDING `Severity::<name>` token: every
// site has the shape `report(Severity::X, loc, "internal: …")`, and the literal
// routinely sits several lines below the severity because the argument list
// wraps.  The recorded `text` runs forward to the statement's `;` so it spans
// every piece of a concatenated message — which is what lets the {file, issue
// tag} key work even when the tag is in the last of five string fragments.
void scanFile(const std::string& rel, const std::string& contents,
              std::vector<Site>& out) {
    std::vector<std::string> lines;
    {
        std::istringstream ls(contents);
        std::string l;
        while (std::getline(ls, l)) lines.push_back(l);
    }
    const std::string needle = "\"internal: ";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(needle) == std::string::npos) continue;
        if (isCommentLine(lines[i])) continue;
        Site s;
        s.file = rel;
        s.line = static_cast<int>(i) + 1;
        // Forward to the end of the report(...) statement. The terminator test
        // is "the trimmed line ENDS with `;`", not "contains a `;`": several of
        // these messages contain a semicolon INSIDE a string literal
        // ("…resolved indexer; the read degrades…"), and a contains-test
        // truncated the scan there and lost the trailing issue tag. That was
        // caught by running this test against a tree it was not written for —
        // the cheapest available proof that the scan is not just agreeing with
        // whatever it happened to be developed on.
        for (std::size_t j = i; j < lines.size() && j < i + 40; ++j) {
            s.text += lines[j];
            s.text += ' ';
            std::size_t last = lines[j].find_last_not_of(" \t\r");
            if (last != std::string::npos && lines[j][last] == ';') break;
        }
        for (std::size_t j = i + 1; j-- > 0 && i - j < 40;) {
            std::size_t sev = lines[j].find("Severity::");
            if (sev == std::string::npos || isCommentLine(lines[j])) continue;
            std::size_t b = sev + std::string("Severity::").size();
            std::size_t e = b;
            while (e < lines[j].size() &&
                   (std::isalnum(static_cast<unsigned char>(lines[j][e])) ||
                    lines[j][e] == '_'))
                ++e;
            s.severity = lines[j].substr(b, e - b);
            break;
        }
        out.push_back(std::move(s));
    }
}

// A RECURSIVE walk, deliberately: a new directory under `src/` that starts
// reporting `internal:` diagnostics must be seen, and an enumerated directory
// list is exactly the "source collection that never held the population"
// failure this snapshot exists to prevent.
std::vector<Site> scanSrcTree(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<Site> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root + "/src", ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string path = it->path().string();
        if (path.size() < 5) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        std::ifstream in(path);
        if (!in.good()) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        scanFile(path.substr(root.size() + 1), ss.str(), out);
    }
    return out;
}

}  // namespace

BEZEL_TEST(internal_degrade_disposition_snapshot_2924,
           every_warning_severity_degrade_has_a_recorded_safety_disposition) {
    const std::string root = repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());

    const std::vector<Site> sites = scanSrcTree(root);
    // The scan must find the population at all — a zero here means the walk or
    // the needle broke, not that the tree became clean.
    BEZEL_EXPECT_TRUE(sites.size() > 22);

    std::vector<const Site*> warnings;
    for (const Site& s : sites) {
        if (s.severity.empty()) {
            std::cout << "    UNCLASSIFIABLE `internal:` site (no Severity:: "
                         "within 40 lines above): "
                      << s.file << ":" << s.line << "\n";
        }
        BEZEL_EXPECT_TRUE(!s.severity.empty());
        if (s.severity == "Warning") warnings.push_back(&s);
    }

    // Every Warning site must be in the snapshot.
    constexpr std::size_t kN = sizeof(kWarningDegrades) / sizeof(Entry);
    std::vector<bool> matched(kN, false);
    for (const Site* s : warnings) {
        bool known = false;
        for (std::size_t k = 0; k < kN; ++k) {
            const Entry& e = kWarningDegrades[k];
            if (s->file == e.file &&
                s->text.find(e.issueTag) != std::string::npos && !matched[k]) {
                matched[k] = true;
                known = true;
                break;
            }
        }
        if (!known) {
            std::cout
                << "    NEW Severity::Warning `internal:` degrade — this "
                   "SHIPS A BINARY:\n      "
                << s->file << ":" << s->line
                << "\n    Decide whether the zero/null it substitutes can land "
                   "in a POINTER something later dereferences (#2922 measured "
                   "exactly that: a null `string` and a SIGSEGV at address "
                   "0x8).\n    If it can, make it an Error where a value is "
                   "DEMANDED; either way record the judgement in "
                   "kWarningDegrades in this file.\n";
        }
        BEZEL_EXPECT_EQ(known, true);
    }

    // And every snapshot entry must still exist — a removed or re-severitied
    // site should retire its entry, not leave a stale claim about the tree.
    for (std::size_t k = 0; k < kN; ++k) {
        if (!matched[k])
            std::cout << "    snapshot lists a Warning degrade that no longer "
                         "exists (retire the entry, or check whether it became "
                         "an Error): "
                      << kWarningDegrades[k].file << " — "
                      << kWarningDegrades[k].issueTag << "\n";
        BEZEL_EXPECT_EQ(matched[k], true);
    }

    BEZEL_EXPECT_EQ(warnings.size(), kN);

    std::size_t unsafePointer = 0;
    for (const Entry& e : kWarningDegrades)
        if (e.disposition == Disposition::UnsafePointer) ++unsafePointer;
    BEZEL_EXPECT_EQ(unsafePointer, kUnsafePointerCount);
}
