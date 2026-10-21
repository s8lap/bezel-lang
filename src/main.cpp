// bezelc — the Bezel compiler driver.
//
// Argument handling is intentionally hand-rolled: adopting a CLI-parsing
// dependency is a project-level decision (CLAUDE.md), and the scaffold needs
// only enough to route a file path into the driver.

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "driver/Driver.hpp"

namespace {

// fix #3084: the --version string is built from a configure-time substitution
// (BEZEL_VERSION, defined in the root CMakeLists.txt via `git describe --tags
// --long --dirty=-modified`). The macro is required — without it the scaffolding
// comment below was the only fix and it drifted from the truth for too long.
#ifndef BEZEL_VERSION
#define BEZEL_VERSION "0.0.0-unknown"
#endif

constexpr const char* kVersion = "bezelc " BEZEL_VERSION;

void printUsage(std::ostream& out) {
    out << "usage: bezelc [--version] [--help] [-c] [-o <output>] "
           "[--import <artifact>]... [--checked-disposal | --unchecked-disposal] "
           "[--checked-contracts] "
           "[--sanitize=address] [--experimental-scoped-await-bind] "
           "[--report-trust-admissions] "
           "[--link=self-contained|system] "
           "<source-file>\n"
           "  -c                 compile to a native object (a library, §2.4) "
           "with embedded\n"
           "                     interface metadata; do not link, no `Main` "
           "required\n"
           "  --import <artifact> a dependency LIBRARY artifact (object) whose "
           "`.bezel.meta`\n"
           "                     is loaded for `import` resolution (§2.5); its "
           "object is\n"
           "                     also linked into the executable. An executable "
           "artifact\n"
           "                     is rejected here (§2.8.4 rule 4)\n"
           "  --checked-disposal enable checked disposal mode (§28.2): "
           "member access on a\n"
           "                     disposed instance raises DisposedAccess"
           "Exception (§31.4)\n"
           "  --unchecked-disposal select unchecked (release) disposal mode "
           "(§28.2): use of a\n"
           "                     disposed instance is undefined behavior; "
           "cannot be combined\n"
           "                     with --checked-disposal\n"
           "  --checked-contracts enable checked-contracts mode (§22.16): "
           "`ensures`\n"
           "                     clauses evaluate at every normal-completion "
           "path and\n"
           "                     `invariant` clauses evaluate at construction "
           "exit and\n"
           "                     public-method entry/exit; a false clause "
           "raises\n"
           "                     ContractViolationException. Without this flag "
           "(the\n"
           "                     §22.16 release default) unproven `ensures`/"
           "invariant\n"
           "                     clauses are NOT emitted at codegen. "
           "`requires` clauses\n"
           "                     are always-on regardless of this flag\n"
           "  --sanitize=address instrument the GENERATED code with LLVM "
           "AddressSanitizer and\n"
           "                     link the executable against the ASan runtime, "
           "so a\n"
           "                     use-after-free / out-of-bounds read in emitted "
           "code is\n"
           "                     caught at the access (JIT→AOT memory net); "
           "executable\n"
           "                     builds only (not -c); needs --link=system\n"
           "  --link=self-contained\n"
           "                     link a fully static executable with the toolchain\n"
           "                     bundle in <prefix>/lib/bezel/x86_64-linux-musl (a\n"
           "                     static ld.lld, musl, libc++): no clang or -dev\n"
           "                     packages needed. The default when that bundle\n"
           "                     exists next to bezelc, as in a release tarball\n"
           "  --link=system      link with `clang` from PATH against the host's\n"
           "                     glibc and libstdc++. The default otherwise\n"
           "  --experimental-scoped-await-bind\n"
           "                     PROTOTYPE (proposal 0082 §3, issue #3048; not a\n"
           "                     language feature): admit `var x := await\n"
           "                     <fresh Task<T> producer>;` by desugaring it to\n"
           "                     `using var $t := <producer>; var x := await $t;`\n"
           "  --report-trust-admissions\n"
           "                     §28.3 *Diagnostics* trust-admission report mode\n"
           "                     (v0.307, issue #4605; off by default): emit a\n"
           "                     warning at every site where the compiler makes an\n"
           "                     admission resting on a K4.3-recorded trust clause —\n"
           "                     a parameter-/receiver-sourced borrow stored into a\n"
           "                     non-owned field, a borrowing-family collection or\n"
           "                     an OwningDictionary key slot, or held by a pending\n"
           "                     frame past its call. Informative only: no verdict\n"
           "                     and no generated code changes, and a `scoped`\n"
           "                     parameter's statically-bounded twin stays silent\n";
}

// Default executable name from the source path: drop the directory and the
// final extension (foo/bar.bzl -> bar). Falls back to "a.out".
std::string defaultOutput(const std::string& input) {
    std::string base = input;
    if (auto slash = base.find_last_of('/'); slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    if (auto dot = base.find_last_of('.'); dot != std::string::npos && dot > 0) {
        base = base.substr(0, dot);
    }
    return base.empty() ? "a.out" : base;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printUsage(std::cerr);
        return 2;
    }

    std::string inputPath;
    std::string outputPath;
    // §2.4 / §2.5 (issue #328 inc 3): `-c` emits a library object; `--import`
    // names a dependency artifact whose interface is loaded and whose object is
    // linked. A dependency's metadata and object live in the same artifact, so
    // one path fills both Dependency fields.
    bool compileObjectOnly = false;
    bezel::codegen::CodegenOptions codegenOpts;
    // §28.2 (#4602): the two disposal-mode switches are mutually exclusive.
    bool sawCheckedDisposal = false;
    bool sawUncheckedDisposal = false;
    std::vector<bezel::Dependency> deps;
    // #6654: the link recipe. Unset = the default, resolved below once every
    // argument is known (self-contained when the bundle is next to bezelc).
    std::optional<bezel::LinkMode> linkMode;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--version") {
            std::cout << kVersion << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
        if (arg == "-o") {
            if (i + 1 >= args.size()) {
                std::cerr << "bezelc: error: -o requires an argument\n";
                return 2;
            }
            outputPath = args[++i];
            continue;
        }
        if (arg == "-c") {
            compileObjectOnly = true;
            continue;
        }
        if (arg == "--checked-disposal") {
            // §28.2 v0.134 / issue #601: enable checked disposal mode. Every
            // instance-member entry emits a disposed-flag test; access on a
            // disposed instance raises DisposedAccessException (§31.4).
            codegenOpts.checkedDisposal = true;
            sawCheckedDisposal = true;
            continue;
        }
        if (arg == "--unchecked-disposal") {
            // §28.2 (proposal 0128 §2.1, #4602): select the unchecked (release)
            // disposal mode explicitly. The default is not flipped here, so this
            // sets the field to the value a switch-less build already uses; it
            // only records the selection. Given together with --checked-disposal
            // it is a usage error (proposal 0128 §9 Q1; changelog v0.307).
            codegenOpts.checkedDisposal = false;
            sawUncheckedDisposal = true;
            continue;
        }
        if (arg == "--checked-contracts") {
            // §22.16 (issue #4386): enable checked-contracts mode. `ensures`
            // clauses evaluate at every normal-completion path; `invariant`
            // clauses evaluate at construction exit and public-method entry/
            // exit; a false clause raises ContractViolationException. Without
            // this flag (the §22.16 release default) the clause evaluations
            // are NOT emitted at codegen — clauses are pure, so non-evaluation
            // is unobservable for any program whose checked clauses never
            // fault. `requires` is always-on regardless of this flag.
            codegenOpts.checkedContracts = true;
            continue;
        }
        if (arg == "--experimental-scoped-await-bind") {
            // Proposal 0082 §3 stage-1 prototype (#3048). Off by default; with
            // the flag absent the compiler is bit-for-bit what it was.
            codegenOpts.scopedAwaitBind = true;
            continue;
        }
        if (arg == "--report-trust-admissions") {
            // §28.3 *Diagnostics* "The trust-admission report" (v0.307, #4605).
            // Off by default and selected explicitly; with the flag absent the
            // compiler is bit-for-bit what it was (the spec's "the default
            // build's diagnostics are unchanged").
            codegenOpts.reportTrustAdmissions = true;
            continue;
        }
        if (arg == "--sanitize=address") {
            // JIT→AOT test-harness migration §2 (memory net): run LLVM's
            // AddressSanitizerPass over the generated module and link the
            // executable against the ASan runtime, so a use-after-free /
            // out-of-bounds read in the emitted code is caught at the access.
            codegenOpts.sanitizeAddress = true;
            continue;
        }
        if (arg.rfind("--link=", 0) == 0) {
            const std::string mode = arg.substr(std::string("--link=").size());
            if (mode == "self-contained") {
                linkMode = bezel::LinkMode::SelfContained;
            } else if (mode == "system") {
                linkMode = bezel::LinkMode::System;
            } else {
                std::cerr << "bezelc: error: unknown link mode '" << mode
                          << "' (expected 'self-contained' or 'system')\n";
                return 2;
            }
            continue;
        }
        if (arg.rfind("--sanitize=", 0) == 0) {
            std::cerr << "bezelc: error: unsupported sanitizer '"
                      << arg.substr(std::string("--sanitize=").size())
                      << "' (only 'address' is supported)\n";
            return 2;
        }
        if (arg == "--import") {
            if (i + 1 >= args.size()) {
                std::cerr << "bezelc: error: --import requires an argument\n";
                return 2;
            }
            const std::string artifact = args[++i];
            bezel::Dependency dep;
            dep.metadataPath = artifact;
            dep.objectPath = artifact;
            deps.push_back(std::move(dep));
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            std::cerr << "bezelc: error: unknown option '" << arg << "'\n";
            printUsage(std::cerr);
            return 2;
        }
        if (!inputPath.empty()) {
            std::cerr << "bezelc: error: more than one input file given\n";
            return 2;
        }
        inputPath = arg;
    }

    if (sawCheckedDisposal && sawUncheckedDisposal) {
        std::cerr << "bezelc: error: --checked-disposal and --unchecked-disposal "
                     "cannot be given together (a build selects one "
                     "disposal-checking mode, §28.2)\n";
        return 2;
    }

    if (inputPath.empty()) {
        printUsage(std::cerr);
        return 2;
    }
    // --sanitize=address instruments the emitted code AND relies on the
    // executable being linked with `-fsanitize=address` (the executable path
    // does this). The `-c` library-object path performs no link, so the flag is
    // not wired there; reject the combination rather than silently ignoring it.
    if (compileObjectOnly && codegenOpts.sanitizeAddress) {
        std::cerr << "bezelc: error: --sanitize=address applies to the "
                     "executable build; it is not supported with -c "
                     "(library object)\n";
        return 2;
    }
    // #6654: resolve the link mode. `-c` performs no link, so it neither
    // probes for the bundle nor validates a --link choice.
    bezel::Driver driver;
    if (!compileObjectOnly) {
        const bezel::LinkMode mode =
            linkMode.value_or(bezel::Driver::selfContainedBundlePresent()
                                  ? bezel::LinkMode::SelfContained
                                  : bezel::LinkMode::System);
        if (mode == bezel::LinkMode::SelfContained) {
            // AddressSanitizer has no musl runtime; the ASan link is the
            // system (clang) recipe's `-fsanitize=address`.
            if (codegenOpts.sanitizeAddress) {
                std::cerr << "bezelc: error: --sanitize=address is not supported "
                             "by the self-contained (musl) link; pass "
                             "--link=system to link with clang and the ASan "
                             "runtime\n";
                return 2;
            }
            const std::string bundle = bezel::Driver::selfContainedBundleDir();
            if (!bezel::Driver::selfContainedBundlePresent()) {
                std::cerr << "bezelc: error: --link=self-contained: no "
                             "self-contained toolchain at '"
                          << bundle
                          << "' (a release tarball ships it in "
                             "lib/bezel/x86_64-linux-musl); pass --link=system "
                             "to link with clang\n";
                return 2;
            }
            driver.setLinkMode(mode, bundle);
        }
    }
    if (outputPath.empty()) outputPath = defaultOutput(inputPath);


    const bezel::CompileResult result =
        compileObjectOnly
            ? driver.compileObject(inputPath, outputPath, deps, codegenOpts)
            : driver.compileFile(inputPath, outputPath, deps, codegenOpts);
    return result.success ? 0 : 1;
}
