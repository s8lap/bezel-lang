// A RATCHET on one C++ lifetime shape in the code generator: a stack
// `sema::TypeRef` holder whose ADDRESS is stored into a pointer declared in an
// OUTER scope, so the pointer outlives the object it points at.
//
// WHY THIS EXISTS.  #3487 was exactly that, twice, in `buildOrderedArgs`
// (src/codegen/LlvmBackend_Assign.cpp).  The shape is:
//
//     const sema::TypeRef* formal0 = ctx_.resolvedType(...);   // outer scope
//     if (formal0) {
//         if (<formal0 is a bare `T?`>) {
//             sema::TypeRef concrHolder;                       // INNER scope
//             if (nullableTypeParamConcrete(formal0, concrHolder))
//                 formal0 = &concrHolder;                      // escapes
//         }
//     }
//     ... unitValueForTarget(v, formal0, ...);                 // reads it DEAD
//
// `nullableTypeParamConcrete` fills the holder BY VALUE and the caller points
// at it, so the holder must outlive every dereference.  It did not.  Measured
// on `origin/main` as `stack-use-after-scope` with a `bezelc` built
// `-fsanitize=address -fsanitize-address-use-after-scope`, from the ordinary
// program `C.Take<()>(C.Nothing())` where `Take<T>(x: T?) where T: value`
// (spec §4.7 "The unit type" / §14.2 "Constraints", v0.283).
//
// ⚠ WHY THIS TEST IS A SOURCE SCAN AND NOT A gtest ROW.  The undefined
// behaviour is in the COMPILER PROCESS, not in the compiled program.  The
// suite's generated-code AddressSanitizer (`gate.sh --full`) instruments the
// programs `bezelc` emits — it cannot see a dangling read inside `bezelc`
// itself, and neither can `runAot`: without ASan the dead stack slot still
// held readable bytes and the repro printed the RIGHT answers (`1`/`0`), which
// is exactly why the bug survived.  So no behavioural oracle in this repo can
// fail on it.  A source-shape ratchet can, and it is the only thing here that
// does: it is measured FAILING (2 hits) on the pre-fix file and PASSING on the
// fixed one.
//
// WHAT IT CHECKS.  For every `sema::TypeRef NAME;` value declaration under
// `src/`, every `TARGET = &NAME;` assignment, where `TARGET`'s own declaration
// is at a SHALLOWER brace depth than `NAME`'s, is a hit.  The audit that
// accompanied the fix ran this over all of `src/` and found 45 such holders
// with 58 address-taking uses; after the fix, zero hits.
//
// ⚠ IF THIS TEST FAILS: do not silence it.  Hoist the holder to the scope that
// outlives every dereference of the pointer, the way
// `Emitter::unitValueForCtorParam` (src/codegen/LlvmBackend_Layout.cpp) already
// does — it keeps its own `sema::TypeRef concr;` alive across the
// `unitValueForTarget` call deliberately, and is the local precedent.
//
// LIMITS, stated so a reader does not over-trust it.  The scan is line-based:
// a declaration or an assignment wrapped across lines is not seen.  It only
// follows the `TARGET = &NAME` form — an address passed straight into a call
// (`f(&NAME)`) is out of scope, because the callee cannot outlive the holder
// unless it stores the pointer, which is a different question.  A `TARGET`
// whose declaration the backward scan cannot find (a member, a parameter) is
// skipped rather than guessed at, and a declaration is only accepted when no
// line between it and the use dips below its own brace depth — so the scan
// cannot reach across a function boundary and report a neighbour's local.  It
// is a ratchet on ONE known-bad shape, not a lifetime checker.
//
// The declaration matcher accepts `NAME;`, `NAME{...}` and `NAME = ...`.  It
// was `;`-only when first written, which left the identical defect written as
// `sema::TypeRef h{};` or `sema::TypeRef h = ...;` invisible to it — ordinary
// modern C++, and an unstated hole in a ratchet's own LIMITS is the failure
// this project keeps paying for.  Closing it cost nothing: 0 additional hits
// across all of `src/`.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "support/repo_root.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Blank out `//` comment tails and the contents of string / character literals
// so brace counting and pattern matching see only code. Block comments are NOT
// tracked; a `{` or `}` inside one would skew the depth for the rest of the
// file, so the depth is only ever COMPARED between two lines of the same file
// and a skew that affects both equally cancels. (Every hit is printed with its
// depths, so a spurious one is diagnosable at a glance.)
std::string stripCommentsAndLiterals(const std::string& line) {
    std::string out;
    const std::size_t n = line.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/') break;
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < n) {
                if (line[i] == '\\') { i += 2; continue; }
                if (line[i] == quote) { ++i; break; }
                ++i;
            }
            continue;  // literal contributes nothing
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// `sema::TypeRef NAME;` — a BY-VALUE holder. A pointer or reference
// declaration (`sema::TypeRef* p`, `sema::TypeRef& r`) does not match, because
// the character after the type name is then not the start of an identifier.
bool matchValueHolderDecl(const std::string& s, std::string& nameOut) {
    static const std::string kTy = "sema::TypeRef";
    std::size_t at = 0;
    while ((at = s.find(kTy, at)) != std::string::npos) {
        std::size_t i = at + kTy.size();
        const std::size_t next = i;
        if (next < s.size() && isIdentChar(s[next])) { at = next; continue; }
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        const std::size_t begin = i;
        while (i < s.size() && isIdentChar(s[i])) ++i;
        if (i == begin) { at += kTy.size(); continue; }
        std::size_t j = i;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        // Accept `NAME;`, `NAME{...}` and `NAME = ...` — brace-init and
        // copy-init declare a value holder exactly as `;` does, and a matcher
        // that only knew `;` was evadable by writing the same defect as
        // `sema::TypeRef h{};` (measured: 0 new hits across all of src/).
        if (j < s.size() && (s[j] == ';' || s[j] == '{' || s[j] == '=')) {
            nameOut = s.substr(begin, i - begin);
            return true;
        }
        at += kTy.size();
    }
    return false;
}

// `TARGET = &NAME` anywhere on the line. Returns the TARGET identifier.
bool matchAddressEscape(const std::string& s, const std::string& name,
                        std::string& targetOut) {
    const std::string needle = "&" + name;
    std::size_t at = 0;
    while ((at = s.find(needle, at)) != std::string::npos) {
        const std::size_t after = at + needle.size();
        if (after < s.size() && isIdentChar(s[after])) { at = after; continue; }
        if (at > 0 && isIdentChar(s[at - 1])) { at = after; continue; }
        // Scan left: whitespace, then `=` (but not `==`, `!=`, `<=`, `>=`),
        // then whitespace, then the target identifier.
        std::size_t i = at;
        while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) --i;
        if (i == 0 || s[i - 1] != '=') { at = after; continue; }
        --i;
        if (i > 0 && (s[i - 1] == '=' || s[i - 1] == '!' || s[i - 1] == '<' ||
                      s[i - 1] == '>' || s[i - 1] == '+' || s[i - 1] == '-')) {
            at = after;
            continue;
        }
        while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) --i;
        const std::size_t end = i;
        while (i > 0 && isIdentChar(s[i - 1])) --i;
        if (i == end) { at = after; continue; }
        targetOut = s.substr(i, end - i);
        return true;
    }
    return false;
}

// Does this line declare `name` as a POINTER local? Recognises
// `sema::TypeRef* name`, `const sema::TypeRef* name`, and `auto* name =`.
bool declaresPointerLocal(const std::string& s, const std::string& name) {
    for (const char* ty : {"sema::TypeRef", "auto"}) {
        const std::string tyStr = ty;
        std::size_t at = 0;
        while ((at = s.find(tyStr, at)) != std::string::npos) {
            std::size_t i = at + tyStr.size();
            if (i < s.size() && isIdentChar(s[i])) { at = i; continue; }
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            if (i >= s.size() || s[i] != '*') { at += tyStr.size(); continue; }
            ++i;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            static const std::string kConst = "const";
            if (s.compare(i, kConst.size(), kConst) == 0) {
                i += kConst.size();
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            }
            const std::size_t begin = i;
            while (i < s.size() && isIdentChar(s[i])) ++i;
            if (s.compare(begin, i - begin, name) == 0 && i > begin) {
                std::size_t j = i;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (j < s.size() && (s[j] == '=' || s[j] == ';')) return true;
            }
            at += tyStr.size();
        }
    }
    return false;
}

struct Hit {
    std::string file;
    int useLine = 0;
    std::string holder;
    int holderLine = 0;
    int holderDepth = 0;
    std::string target;
    int targetLine = 0;
    int targetDepth = 0;
    std::string text;
};

// How far back to look for the target pointer's declaration. Generous enough
// for the longest commented block in the codegen sources; a target declared
// further back than this is skipped, not guessed at.
constexpr int kBackScanLines = 400;

void scanFile(const fs::path& p, const std::string& rel,
              std::vector<Hit>& hits) {
    std::ifstream in(p);
    if (!in) return;
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(in, line)) raw.push_back(line);

    // Brace depth at the START of each line.
    std::vector<int> depth(raw.size(), 0);
    std::vector<std::string> code(raw.size());
    int d = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        depth[i] = d;
        code[i] = stripCommentsAndLiterals(raw[i]);
        for (char c : code[i]) {
            if (c == '{') ++d;
            else if (c == '}') --d;
        }
    }

    for (std::size_t i = 0; i < raw.size(); ++i) {
        std::string holder;
        if (!matchValueHolderDecl(code[i], holder)) continue;
        const int holderDepth = depth[i];
        // Every `TARGET = &holder` from here to the END OF THE HOLDER'S BLOCK.
        // The running minimum depth is what bounds that block: the first line
        // whose depth drops below the declaration's is past the closing brace,
        // and any `&holder` after it names a DIFFERENT variable in a different
        // function. (A redeclaration of the same name also ends the scan.)
        int liveMin = holderDepth;
        for (std::size_t u = i + 1; u < raw.size(); ++u) {
            liveMin = std::min(liveMin, depth[u]);
            if (liveMin < holderDepth) break;  // holder's block has closed
            std::string again;
            if (matchValueHolderDecl(code[u], again) && again == holder) break;
            std::string target;
            if (!matchAddressEscape(code[u], holder, target)) continue;
            int targetLine = -1;
            int targetDepth = 0;
            // Same bound, walked backwards: the target's declaration counts
            // only if no line between it and the use dips below its own depth.
            // Without that, the scan would happily reach into the PREVIOUS
            // function and report its shallow local as an outliving target.
            int backMin = depth[u];
            for (std::size_t j = u + 1; j-- > 0;) {
                backMin = std::min(backMin, depth[j]);
                if (declaresPointerLocal(code[j], target)) {
                    if (depth[j] <= backMin) {
                        targetLine = static_cast<int>(j) + 1;
                        targetDepth = depth[j];
                    }
                    break;
                }
                if (static_cast<int>(u - j) > kBackScanLines) break;
            }
            if (targetLine < 0) continue;  // member / parameter: not our shape
            if (targetDepth >= holderDepth) continue;  // cannot outlive it
            Hit h;
            h.file = rel;
            h.useLine = static_cast<int>(u) + 1;
            h.holder = holder;
            h.holderLine = static_cast<int>(i) + 1;
            h.holderDepth = holderDepth;
            h.target = target;
            h.targetLine = targetLine;
            h.targetDepth = targetDepth;
            h.text = raw[u];
            hits.push_back(h);
        }
    }
}

}  // namespace

// Cites the operating discipline (CLAUDE.md "the implementation is wrong when
// it disagrees with the spec" does not reach a C++ lifetime rule), plus the
// spec sections the defective code implements: §4.7 "The unit type" and §14.2
// "Constraints", v0.283.
BEZEL_TEST(typeref_holder_scope_escape_3487,
           no_stack_typeref_holder_escapes_into_an_outer_scoped_pointer) {
    const std::string root = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    if (root.empty()) return;

    const std::string src = root + "/src";
    std::vector<Hit> hits;
    std::size_t filesScanned = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".cpp" && p.extension() != ".hpp") continue;
        ++filesScanned;
        scanFile(p, p.string().substr(src.size() + 1), hits);
    }

    // The scan must actually have run; an empty walk would pass vacuously.
    BEZEL_EXPECT_TRUE(filesScanned > 20);

    for (const Hit& h : hits) {
        std::cout << "    stack sema::TypeRef escapes its scope: " << h.file
                  << ":" << h.useLine << "\n      " << h.text
                  << "\n      holder `" << h.holder << "` declared at line "
                  << h.holderLine << " (brace depth " << h.holderDepth
                  << "), but `" << h.target << "` is declared at line "
                  << h.targetLine << " (brace depth " << h.targetDepth
                  << ") and outlives it.\n"
                     "      Hoist the holder to the target's scope — see"
                     " Emitter::unitValueForCtorParam"
                     " (src/codegen/LlvmBackend_Layout.cpp) for the correct"
                     " shape, and #3487 for what happens when it is not.\n";
    }
    BEZEL_EXPECT_EQ(hits.size(), static_cast<std::size_t>(0));
}
