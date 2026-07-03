// Parser tests for the `scoped` contextual modifier — spec §28.14 / §22.3 /
// A.4 ParamModifiers / A.6 FuncParamMods (v0.175, #808).
//
// §22.3: "`scoped` — on a *function-typed* parameter only: the callee may
// store the callback, bounded by the storing object's lifetime." A.4 adds
// `"scoped"` to ParamModifiers; A.6 adds it to FuncParamMods. §28.14 defines
// the `scoped` FIELD ("A `scoped` field holds such a callback"). §32:
// "`scoped` function-typed parameters are ILLEGAL on `extern` declarations."
//
// `scoped` is CONTEXTUAL (lexed as Identifier, not reserved): a parameter, a
// field, or a named type spelled `scoped` must keep parsing as before. The
// function-typed-only restriction is sema's (alias-transparent) and is tested
// in tests/sema; here we assert only the recorded modifier strings.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using namespace bezel;

namespace {

const ast::TypeDecl* firstType(const bezel::test::ParseResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& d : r.sourceFile->topDecls)
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(d.get())) return td;
    return nullptr;
}

const ast::MethodDecl* firstMethod(const bezel::test::ParseResult& r) {
    const ast::TypeDecl* td = firstType(r);
    if (!td) return nullptr;
    for (const auto& m : td->members)
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(m.get())) return md;
    return nullptr;
}

}  // namespace

// §22.3 / A.4 (v0.175): `scoped` on a function-typed method parameter is
// recorded as the parameter's modifier — the spec's own signature shape,
// `method Spawn(scoped body: () -> Task)` (§28.14).
BEZEL_TEST(scoped_modifier_2814, method_param_scoped_recorded_spec_22_3) {
    auto r = parseAll(
        "module M; class C "
        "  public method Spawn(scoped body: () -> ()) end Spawn; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const ast::MethodDecl* md = firstMethod(r);
    BEZEL_EXPECT_TRUE(md != nullptr && md->params.size() == 1);
    if (md && md->params.size() == 1) {
        BEZEL_EXPECT_EQ(std::string(md->params[0].modifier), "scoped");
        BEZEL_EXPECT_EQ(std::string(md->params[0].name), "body");
    }
}

// §22.3 / A.4: `scoped` on a constructor parameter (§28.14 bounding form 1 —
// the constructed object is the bounding object).
BEZEL_TEST(scoped_modifier_2814, ctor_param_scoped_recorded_spec_28_14) {
    auto r = parseAll(
        "module M; class C "
        "  public constructor(scoped cb: () -> ()) end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const ast::TypeDecl* td = firstType(r);
    const ast::ConstructorDecl* cd = nullptr;
    if (td)
        for (const auto& m : td->members)
            if ((cd = dynamic_cast<const ast::ConstructorDecl*>(m.get())))
                break;
    BEZEL_EXPECT_TRUE(cd != nullptr && cd->params.size() == 1);
    if (cd && cd->params.size() == 1)
        BEZEL_EXPECT_EQ(std::string(cd->params[0].modifier), "scoped");
}

// §24.3 (v0.175): the migrated event signature parses — an interface method
// with a scoped handler parameter and an owned Subscription return.
BEZEL_TEST(scoped_modifier_2814, interface_method_scoped_param_spec_24_3) {
    auto r = parseAll(
        "module M; interface IEvt "
        "  method Subscribe(scoped handler: (int) -> ()): owned Subscription; "
        "end IEvt;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 (v0.175): the `scoped` FIELD modifier is recorded alongside the
// other field modifiers. (A.4 FieldDecl's grammar line omits `scoped` — a
// v0.175 docs gap tracked separately — but §28.14's prose defines the scoped
// field normatively.)
BEZEL_TEST(scoped_modifier_2814, field_scoped_modifier_recorded_spec_28_14) {
    auto r = parseAll(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const ast::TypeDecl* td = firstType(r);
    const ast::FieldDecl* fd = nullptr;
    if (td)
        for (const auto& m : td->members)
            if ((fd = dynamic_cast<const ast::FieldDecl*>(m.get()))) break;
    BEZEL_EXPECT_TRUE(fd != nullptr);
    bool sawScoped = false;
    if (fd)
        for (auto mod : fd->modifiers)
            if (mod == "scoped") sawScoped = true;
    BEZEL_EXPECT_TRUE(sawScoped);
}

// A.6 FuncParamMods (v0.175): `scoped` inside a FUNCTION TYPE —
// `(scoped () -> ()) -> ()` records the modifier on the function type's
// first parameter (§27.2 identity depends on this).
BEZEL_TEST(scoped_modifier_2814, func_type_scoped_position_recorded_a6) {
    auto r = parseAll(
        "module M; class C "
        "  private _g: ((scoped () -> ()) -> ())?; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const ast::TypeDecl* td = firstType(r);
    const ast::FieldDecl* fd = nullptr;
    if (td)
        for (const auto& m : td->members)
            if ((fd = dynamic_cast<const ast::FieldDecl*>(m.get()))) break;
    BEZEL_EXPECT_TRUE(fd != nullptr && fd->type != nullptr);
    if (fd && fd->type) {
        BEZEL_EXPECT_TRUE(fd->type->kind == ast::TypeKind::Function);
        BEZEL_EXPECT_TRUE(fd->type->funcParams.size() == 1);
        if (fd->type->funcParams.size() == 1)
            BEZEL_EXPECT_EQ(std::string(fd->type->funcParams[0].modifier),
                            "scoped");
    }
}

// CONTEXTUAL control: a parameter NAMED `scoped` (followed by `:`) is a plain
// parameter, not the modifier — `scoped` is not a reserved keyword (B.1 is
// unchanged by v0.175).
BEZEL_TEST(scoped_modifier_2814, param_named_scoped_still_parses) {
    auto r = parseAll(
        "module M; class C "
        "  public method M(scoped: int) end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const ast::MethodDecl* md = firstMethod(r);
    BEZEL_EXPECT_TRUE(md != nullptr && md->params.size() == 1);
    if (md && md->params.size() == 1) {
        BEZEL_EXPECT_TRUE(md->params[0].modifier.empty());
        BEZEL_EXPECT_EQ(std::string(md->params[0].name), "scoped");
    }
}

// CONTEXTUAL control: a function-type parameter of NAMED TYPE `scoped`
// (`(scoped) -> ()`) is a type reference, not the modifier.
BEZEL_TEST(scoped_modifier_2814, func_type_named_type_scoped_still_parses) {
    auto r = parseAll(
        "module M; class C "
        "  private _g: ((scoped) -> ())?; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const ast::TypeDecl* td = firstType(r);
    const ast::FieldDecl* fd = nullptr;
    if (td)
        for (const auto& m : td->members)
            if ((fd = dynamic_cast<const ast::FieldDecl*>(m.get()))) break;
    BEZEL_EXPECT_TRUE(fd != nullptr && fd->type != nullptr);
    if (fd && fd->type && fd->type->kind == ast::TypeKind::Function &&
        fd->type->funcParams.size() == 1) {
        BEZEL_EXPECT_TRUE(fd->type->funcParams[0].modifier.empty());
    }
}

// §32 (v0.175): `scoped` is ILLEGAL on an extern declaration — neither the
// storage obligations nor quiescence can be verified across the FFI boundary.
BEZEL_TEST(scoped_modifier_2814, extern_scoped_param_rejected_spec_32) {
    auto r = parseAll(
        "module M; "
        "extern \"libc\" "
        "  method Register(scoped cb: (int) -> int); "
        "end;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cites = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("extern") != std::string::npos &&
            d.message.find("scoped") != std::string::npos)
            cites = true;
    BEZEL_EXPECT_TRUE(cites);
}

// §32 control: the same extern declaration WITHOUT `scoped` parses cleanly.
BEZEL_TEST(scoped_modifier_2814, extern_plain_function_param_accepted) {
    auto r = parseAll(
        "module M; "
        "extern \"libc\" "
        "  method Register(cb: (int) -> int); "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
