// Module public-interface metadata BUILDER + EMIT-HOOK tests (§2.5 / §2.6,
// issue #328 increment 2).
//
// Increment 1 unit-tested the schema + serialize/deserialize against hand-built
// fixtures. Increment 2 builds the SAME schema from the LIVE sema symbol table
// of a compiled module, and embeds the serialized blob into the object artifact.
// These tests verify both directions:
//
//   1. buildModuleMetadata(): real Symbols → ModuleMetadata, with the §13
//      public-surface filter — exported types are `public` only (§13.2),
//      members are `public`/`protected` only (§13.1: the inheritance contract),
//      private/internal members excluded. Signatures (types, params, modifiers,
//      generics) come through from the resolved TypeRefs.
//   2. embedModuleMetadata() → object section round-trip: compile a small module
//      through the real AOT path (front-end → LLVM object), read the
//      `.bezel.meta` section back out of the produced object with objcopy, and
//      deserialize() it — asserting the recovered public interface matches and
//      private members are absent.

#include "module/ModuleMetadataBuilder.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ast/Node.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleMetadata.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

// Run lex + parse + symbol-collection + type-resolution + expr-typecheck on a
// source string, then build metadata from the resulting SemaContext. Mirrors
// the driver's front-end ordering (Driver.cpp) so the builder sees the same
// resolved side tables a real compile would.
struct BuiltMeta {
    DiagnosticEngine diagnostics;
    std::unique_ptr<ast::SourceFile> sourceFile;
    std::unique_ptr<sema::SemaContext> ctx;
    ModuleMetadata meta;
    bool ok = false;
};

std::unique_ptr<BuiltMeta> buildFromSource(std::string_view source) {
    auto r = std::make_unique<BuiltMeta>();
    Lexer lexer(source, r->diagnostics);
    Parser parser(lexer, r->diagnostics);
    r->sourceFile = parser.parseCompilationUnit();
    if (!r->sourceFile) return r;
    r->ctx = std::make_unique<sema::SemaContext>(r->diagnostics);
    sema::collectSymbols(*r->sourceFile, *r->ctx);
    sema::resolveTypes(*r->sourceFile, *r->ctx);
    sema::typecheckExpressions(*r->sourceFile, *r->ctx);
    r->meta = buildModuleMetadata(*r->ctx, source);
    r->ok = !r->diagnostics.hasErrors();
    return r;
}

// Find a type decl by name in the metadata (nullptr if absent / excluded).
const MetaTypeDecl* findType(const ModuleMetadata& md, const std::string& n) {
    for (const auto& t : md.types)
        if (t.name == n) return &t;
    return nullptr;
}

// Find a member by name within a type (nullptr if absent / excluded).
const MetaMember* findMember(const MetaTypeDecl& t, const std::string& n) {
    for (const auto& m : t.members)
        if (m.name == n) return &m;
    return nullptr;
}

} // namespace

// §13.2: a top-level type defaults to `internal` (module-private) and is NOT
// part of the exported interface; only `public` top-level types are exported.
BEZEL_TEST(module_metadata_builder, only_public_types_exported_spec_13_2) {
    auto r = buildFromSource(
        "module Lib; "
        "public class Visible "
        "  public method M(): int => 0; "
        "end Visible; "
        "class Hidden "  // implicitly internal — not exported
        "  public method M(): int => 0; "
        "end Hidden;");
    BEZEL_EXPECT_TRUE(r->ctx != nullptr);
    BEZEL_EXPECT_EQ(r->meta.moduleName, std::string("Lib"));
    BEZEL_EXPECT_TRUE(findType(r->meta, "Visible") != nullptr);
    BEZEL_EXPECT_TRUE(findType(r->meta, "Hidden") == nullptr);
    // exports list mirrors the exported types.
    BEZEL_EXPECT_TRUE(std::find(r->meta.exports.begin(), r->meta.exports.end(),
                                "Visible") != r->meta.exports.end());
    BEZEL_EXPECT_TRUE(std::find(r->meta.exports.begin(), r->meta.exports.end(),
                                "Hidden") == r->meta.exports.end());
}

// §13.1 / §13.3: within an exported type, `public` and `protected` members are
// part of the interface (public = anyone, protected = the inheritance contract
// visible to derived classes in other modules); `private` is excluded.
BEZEL_TEST(module_metadata_builder, member_visibility_filter_spec_13_1) {
    auto r = buildFromSource(
        "module Lib; "
        "public class Account "
        "  public Id: int; "                  // exported
        "  protected Region: int; "           // exported (inheritance surface)
        "  private secret: long; "            // excluded
        "  public method Balance(): int => Id; "       // exported
        "  private method recompute(): int => 0; "     // excluded
        "end Account;");
    const MetaTypeDecl* acct = findType(r->meta, "Account");
    BEZEL_EXPECT_TRUE(acct != nullptr);
    if (!acct) return;
    BEZEL_EXPECT_TRUE(findMember(*acct, "Id") != nullptr);
    BEZEL_EXPECT_TRUE(findMember(*acct, "Region") != nullptr);
    BEZEL_EXPECT_TRUE(findMember(*acct, "Balance") != nullptr);
    // §13.1: private members are not bindable downstream → must be excluded.
    BEZEL_EXPECT_TRUE(findMember(*acct, "secret") == nullptr);
    BEZEL_EXPECT_TRUE(findMember(*acct, "recompute") == nullptr);

    // The protected member carries its visibility for the loader to enforce §13.
    if (const MetaMember* region = findMember(*acct, "Region"))
        BEZEL_EXPECT_TRUE(region->visibility == MetaVisibility::Protected);
}

// §3 / §4: a field/method's signature types come through resolved — a primitive
// field is a Primitive leaf, a named (user-type) field carries the type name.
BEZEL_TEST(module_metadata_builder, signature_types_resolved_spec_3) {
    auto r = buildFromSource(
        "module Lib; "
        "public class Pair "
        "  public First: int; "
        "  public Second: string; "
        "  public Other: Pair?; "        // §3.3 nullable named type
        "end Pair;");
    const MetaTypeDecl* pair = findType(r->meta, "Pair");
    BEZEL_EXPECT_TRUE(pair != nullptr);
    if (!pair) return;
    if (const MetaMember* f = findMember(*pair, "First")) {
        BEZEL_EXPECT_TRUE(f->type.kind == TypeKind::Primitive);
        BEZEL_EXPECT_TRUE(f->type.primitive == PrimitiveKind::Int);
    }
    if (const MetaMember* s = findMember(*pair, "Second")) {
        BEZEL_EXPECT_TRUE(s->type.kind == TypeKind::Primitive);
        BEZEL_EXPECT_TRUE(s->type.primitive == PrimitiveKind::String);
    }
    if (const MetaMember* o = findMember(*pair, "Other")) {
        BEZEL_EXPECT_TRUE(o->type.kind == TypeKind::Named);
        BEZEL_EXPECT_EQ(o->type.namedName, std::string("Pair"));
        BEZEL_EXPECT_TRUE(o->type.nullable);
    }
}

// §22 method signature: return type + parameter modifiers + parameter types
// survive the build. ref/out (§22) on params is part of the callable contract.
BEZEL_TEST(module_metadata_builder, method_signature_built_spec_22) {
    auto r = buildFromSource(
        "module Lib; "
        "public class Calc "
        "  public method Add(a: int, b: int): int => a + b; "
        "  public method TryParse(s: string, ref ok: bool): int => 0; "
        "end Calc;");
    const MetaTypeDecl* calc = findType(r->meta, "Calc");
    BEZEL_EXPECT_TRUE(calc != nullptr);
    if (!calc) return;
    if (const MetaMember* add = findMember(*calc, "Add")) {
        BEZEL_EXPECT_TRUE(add->kind == MetaMemberKind::Method);
        BEZEL_EXPECT_TRUE(add->type.kind == TypeKind::Primitive);
        BEZEL_EXPECT_TRUE(add->type.primitive == PrimitiveKind::Int);
        BEZEL_EXPECT_EQ(add->params.size(), std::size_t{2});
        if (add->params.size() == 2) {
            BEZEL_EXPECT_EQ(add->params[0].name, std::string("a"));
            BEZEL_EXPECT_TRUE(add->params[0].type.primitive ==
                              PrimitiveKind::Int);
        }
    }
    if (const MetaMember* tp = findMember(*calc, "TryParse")) {
        BEZEL_EXPECT_EQ(tp->params.size(), std::size_t{2});
        if (tp->params.size() == 2)
            BEZEL_EXPECT_EQ(tp->params[1].modifier, std::string("ref"));
    }
}

// §14: a generic type's parameters (with variance) come through; an interface's
// members are implicitly public (§13.3) and all exported.
BEZEL_TEST(module_metadata_builder, generics_and_interface_spec_14) {
    auto r = buildFromSource(
        "module Lib; "
        "public interface ISink<in T> "
        "  method Accept(item: T): bool; "  // interface member: implicitly public
        "end ISink;");
    const MetaTypeDecl* sink = findType(r->meta, "ISink");
    BEZEL_EXPECT_TRUE(sink != nullptr);
    if (!sink) return;
    BEZEL_EXPECT_TRUE(sink->kind == MetaTypeDeclKind::Interface);
    BEZEL_EXPECT_EQ(sink->genericParams.size(), std::size_t{1});
    if (!sink->genericParams.empty()) {
        BEZEL_EXPECT_EQ(sink->genericParams[0].name, std::string("T"));
        BEZEL_EXPECT_EQ(sink->genericParams[0].variance, std::string("in"));
    }
    // Interface member has no visibility keyword but is exported (§13.3).
    BEZEL_EXPECT_TRUE(findMember(*sink, "Accept") != nullptr);
}

// §10: enum constants are exported with their underlying values (§10.1 implicit
// from 0; §10.2 explicit, with continuation).
BEZEL_TEST(module_metadata_builder, enum_constants_built_spec_10) {
    auto r = buildFromSource(
        "module Lib; "
        "public enum HttpStatus "
        "  Ok := 200, "
        "  Created, "       // §10.2: continues from previous explicit → 201
        "  NotFound := 404; "
        "end HttpStatus;");
    const MetaTypeDecl* st = findType(r->meta, "HttpStatus");
    BEZEL_EXPECT_TRUE(st != nullptr);
    if (!st) return;
    BEZEL_EXPECT_TRUE(st->kind == MetaTypeDeclKind::Enum);
    if (const MetaMember* ok = findMember(*st, "Ok")) {
        BEZEL_EXPECT_TRUE(ok->kind == MetaMemberKind::EnumConstant);
        BEZEL_EXPECT_TRUE(ok->hasEnumValue);
        BEZEL_EXPECT_EQ(ok->enumValue, std::int64_t{200});
    }
    if (const MetaMember* cr = findMember(*st, "Created")) {
        BEZEL_EXPECT_TRUE(cr->hasEnumValue);
        BEZEL_EXPECT_EQ(cr->enumValue, std::int64_t{201});  // §10.2 continuation
    }
    if (const MetaMember* nf = findMember(*st, "NotFound")) {
        BEZEL_EXPECT_TRUE(nf->hasEnumValue);
        BEZEL_EXPECT_EQ(nf->enumValue, std::int64_t{404});
    }
}

// §35 (v0.200, #2044) / #2121: a public user-declared choice in module
// metadata is recorded with `MetaTypeDeclKind::Choice`, not the silent
// `Class` fallback that the previous `-Wswitch` arm omission produced. The
// enum byte (now `Choice = 4`) flows through serialize / deserialize so a
// cross-module importer that branches on the kind takes the choice path.
BEZEL_TEST(module_metadata_builder, choice_kind_recorded_spec_35_2121) {
    auto r = buildFromSource(
        "module Lib; "
        "public choice Color Red, Green, Blue; end Color;");
    BEZEL_EXPECT_TRUE(r->ok);
    const MetaTypeDecl* cc = findType(r->meta, "Color");
    BEZEL_EXPECT_TRUE(cc != nullptr);
    if (!cc) return;
    BEZEL_EXPECT_TRUE(cc->kind == MetaTypeDeclKind::Choice);
    // The wire byte must match the enum value — pin the wire format so a
    // future case-serialization layer can extend the schema without shifting
    // the choice-kind byte out from under existing readers.
    std::vector<std::uint8_t> bytes = serialize(r->meta);
    BEZEL_EXPECT_TRUE(!bytes.empty());
    std::optional<ModuleMetadata> decoded = deserialize(bytes);
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (!decoded) return;
    const MetaTypeDecl* dcc = findType(*decoded, "Color");
    BEZEL_EXPECT_TRUE(dcc != nullptr);
    if (!dcc) return;
    BEZEL_EXPECT_TRUE(dcc->kind == MetaTypeDeclKind::Choice);
    BEZEL_EXPECT_TRUE(dcc->kind == cc->kind);
}

// §2.5/§2.6: built metadata survives serialize → deserialize unchanged (the
// builder produces a schema the Increment-1 codec round-trips).
BEZEL_TEST(module_metadata_builder, built_metadata_roundtrips_spec_2_5) {
    auto r = buildFromSource(
        "module Lib.Demo; "
        "public class C "
        "  public X: int; "
        "  private y: int; "
        "  public method M(n: int): int => n; "
        "end C; "
        "public record P(Amount: int, Name: string); ");
    std::vector<std::uint8_t> bytes = serialize(r->meta);
    BEZEL_EXPECT_TRUE(!bytes.empty());
    std::optional<ModuleMetadata> decoded = deserialize(bytes);
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (!decoded) return;
    BEZEL_EXPECT_TRUE(*decoded == r->meta);
    // The header semantic hash matches the content hash of the built metadata.
    std::optional<std::uint64_t> hdrHash = readSemanticHash(bytes);
    BEZEL_EXPECT_TRUE(hdrHash.has_value());
    if (hdrHash) BEZEL_EXPECT_EQ(*hdrHash, semanticHash(r->meta));
}

// --- Round-trip OUT OF A REAL OBJECT (the increment's headline test) ---------
//
// §2.6 step 4: compile a small module through the real AOT path (Driver →
// LLVM object), then pull the `.bezel.meta` section back out of the produced
// object with objcopy and deserialize() it. The recovered public interface must
// match what the builder produced, and private members must be absent.

namespace {

// Extract the named section from `objectPath` into a byte vector via objcopy
// (the project toolchain ships binutils objcopy; --dump-section writes the raw
// section contents). Returns empty on failure.
std::vector<std::uint8_t> dumpSection(const std::string& objectPath,
                                      const std::string& section) {
    auto dir = std::filesystem::temp_directory_path();
    std::string out =
        (dir / ("bezel_meta_dump_" + std::to_string(std::rand()) + ".bin"))
            .string();
    std::string cmd = "objcopy --dump-section " + section + "=" + out + " " +
                      objectPath + " /dev/null 2>/dev/null";
    std::vector<std::uint8_t> bytes;
    if (std::system(cmd.c_str()) == 0) {
        std::ifstream in(out, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string s = ss.str();
            bytes.assign(s.begin(), s.end());
        }
    }
    std::remove(out.c_str());
    return bytes;
}

} // namespace

BEZEL_TEST(module_metadata_emit, object_section_roundtrip_spec_2_6) {
    // A representative library: a public class with a public + private member,
    // and an internal (non-exported) type. After compilation the object must
    // carry the public interface — and only the public surface.
    const char* source =
        "module Bank.Core; "
        "public class Account "
        "  public Id: int; "
        "  private balance: long; "          // §13.1: excluded from interface
        "  public method Deposit(amount: int): int => Id; "
        "end Account; "
        "class Internals "                    // §13.2 internal: not exported
        "  public method Helper(): int => 0; "
        "end Internals; "
        "public class App "
        "  public static method Main(): int => 0; "
        "end App;";

    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_meta_obj_" + std::to_string(std::rand());
    std::string srcPath = (dir / (stem + ".bzl")).string();
    std::string exePath = (dir / stem).string();
    std::string objPath = exePath + ".o";  // Driver writes outputPath + ".o"

    {
        std::ofstream out(srcPath);
        out << source;
    }

    // The driver deletes the .o after linking, so keep a copy: compile, then
    // immediately re-derive the object via the driver but intercept the .o.
    // Simplest: run the driver, which (on success) links and removes the .o.
    // To capture the .o we compile, copy the .o before it is removed is racy;
    // instead we re-emit the object directly is not exposed. So we read the
    // metadata from the LINKED EXECUTABLE, which also carries the section
    // (the linker preserves non-allocated SHT_PROGBITS sections by name).
    bezel::Driver driver;
    bezel::CompileResult cr = driver.compileFile(srcPath, exePath);
    BEZEL_EXPECT_TRUE(cr.success);

    std::vector<std::uint8_t> sectionBytes;
    if (cr.success) {
        // The .bezel.meta section is carried through the link into the exe.
        sectionBytes = dumpSection(exePath, kMetadataSectionName);
    }
    BEZEL_EXPECT_TRUE(!sectionBytes.empty());

    std::optional<ModuleMetadata> recovered;
    if (!sectionBytes.empty()) recovered = deserialize(sectionBytes);
    BEZEL_EXPECT_TRUE(recovered.has_value());

    if (recovered) {
        BEZEL_EXPECT_EQ(recovered->moduleName, std::string("Bank.Core"));
        // Public types present; internal type absent (§13.2).
        BEZEL_EXPECT_TRUE(findType(*recovered, "Account") != nullptr);
        BEZEL_EXPECT_TRUE(findType(*recovered, "App") != nullptr);
        BEZEL_EXPECT_TRUE(findType(*recovered, "Internals") == nullptr);
        // Public member present, private member absent (§13.1).
        if (const MetaTypeDecl* acct = findType(*recovered, "Account")) {
            BEZEL_EXPECT_TRUE(findMember(*acct, "Id") != nullptr);
            BEZEL_EXPECT_TRUE(findMember(*acct, "Deposit") != nullptr);
            BEZEL_EXPECT_TRUE(findMember(*acct, "balance") == nullptr);
        }
        // The source declares App.Main, so the driver stamped the artifact
        // executable-kind (#1733, §2.8.4); verify that survived the roundtrip.
        BEZEL_EXPECT_TRUE(recovered->isExecutable);
        // The recovered metadata equals a fresh build from the same source. The
        // metadata-only buildFromSource helper does not run the driver's
        // output-mode decision, so it leaves isExecutable at its default —
        // normalize that one field (it is orthogonal to the public-interface
        // roundtrip this test verifies).
        auto fresh = buildFromSource(source);
        fresh->meta.isExecutable = recovered->isExecutable;
        BEZEL_EXPECT_TRUE(*recovered == fresh->meta);
    }

    std::remove(srcPath.c_str());
    std::remove(exePath.c_str());
    std::remove(objPath.c_str());
}

// #2054 / spec §2.8.2 (v0.204): the builder carries purity, authored
// clauses, and class invariants. The primary spelling uses v0.207's structural
// clause parentheses; the legacy fallback keeps this fixture runnable on a
// pre-#2078 checkout while the implementation train is being integrated.
BEZEL_TEST(module_metadata_builder,
           contract_carriage_and_positional_text_spec_2_8_2_2054) {
    const std::string modern =
        "module Lib; "
        "public class Account "
        "  private _balance: int := 0; "
        "  invariant(this._balance >= 0); "
        "  public pure method Withdraw(amount: int): int "
        "    requires(amount > 0) "
        "    ensures(amount > 1) "
        "    ensures(1 = 1) "
        "    return amount; "
        "  end Withdraw; "
        "end Account;";
    const std::string legacy =
        "module Lib; "
        "public class Account "
        "  private _balance: int := 0; "
        "  invariant(this._balance >= 0); "
        "  public pure method Withdraw(amount: int): int "
        "    requires(amount > 0); "
        "    ensures(amount > 1); "
        "    ensures(1 = 1); "
        "    return amount; "
        "  end Withdraw; "
        "end Account;";

    auto r = buildFromSource(modern);
    if (!r->ctx || r->diagnostics.hasErrors()) r = buildFromSource(legacy);
    BEZEL_EXPECT_TRUE(r->ctx != nullptr);
    const MetaTypeDecl* account = findType(r->meta, "Account");
    BEZEL_EXPECT_TRUE(account != nullptr);
    if (!account) return;
    const MetaMember* withdraw = findMember(*account, "Withdraw");
    BEZEL_EXPECT_TRUE(withdraw != nullptr);
    if (!withdraw) return;

    BEZEL_EXPECT_TRUE(r->meta.contractRecordsCarried);
    BEZEL_EXPECT_TRUE(withdraw->isPure);
    BEZEL_EXPECT_EQ(withdraw->contracts.size(), std::size_t{3});
    if (withdraw->contracts.size() == 3) {
        BEZEL_EXPECT_TRUE(withdraw->contracts[0].kind ==
                          MetaContractKind::Requires);
        BEZEL_EXPECT_TRUE(withdraw->contracts[1].kind ==
                          MetaContractKind::Ensures);
        BEZEL_EXPECT_TRUE(withdraw->contracts[0].tier ==
                          MetaContractTier::Checked);
        BEZEL_EXPECT_TRUE(withdraw->contracts[2].tier ==
                          MetaContractTier::Proven);
        BEZEL_EXPECT_TRUE(withdraw->contracts[0].canonicalText.find("%0") !=
                          std::string::npos);
        BEZEL_EXPECT_TRUE(withdraw->contracts[0].canonicalText.find("amount") ==
                          std::string::npos);
    }
    BEZEL_EXPECT_EQ(account->invariants.size(), std::size_t{1});
    if (!account->invariants.empty()) {
        BEZEL_EXPECT_TRUE(account->invariants[0].kind ==
                          MetaContractKind::Invariant);
        BEZEL_EXPECT_TRUE(!account->invariants[0].canonicalText.empty());
        BEZEL_EXPECT_TRUE(account->invariants[0].canonicalText.find(
                              "Lib.Account._balance") != std::string::npos);
    }

    auto decoded = deserialize(serialize(r->meta));
    BEZEL_EXPECT_TRUE(decoded.has_value());
    if (decoded) BEZEL_EXPECT_TRUE(*decoded == r->meta);
}

} // namespace bezel::module
