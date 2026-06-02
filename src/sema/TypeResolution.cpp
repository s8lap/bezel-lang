#include "sema/TypeResolution.hpp"

#include <unordered_map>
#include <unordered_set>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

PrimitiveType primitiveFromName(std::string_view name) {
    if (name == "bool")    return PrimitiveType::Bool;
    if (name == "char")    return PrimitiveType::Char;
    if (name == "string")  return PrimitiveType::String;
    if (name == "byte")    return PrimitiveType::Byte;
    if (name == "sbyte")   return PrimitiveType::SByte;
    if (name == "short")   return PrimitiveType::Short;
    if (name == "ushort")  return PrimitiveType::UShort;
    if (name == "int")     return PrimitiveType::Int;
    if (name == "uint")    return PrimitiveType::UInt;
    if (name == "long")    return PrimitiveType::Long;
    if (name == "ulong")   return PrimitiveType::ULong;
    if (name == "float")   return PrimitiveType::Float;
    if (name == "double")  return PrimitiveType::Double;
    if (name == "decimal") return PrimitiveType::Decimal;
    if (name == "Object")  return PrimitiveType::Object;
    return PrimitiveType::None;
}

namespace {

// Lookup environment in scope. A stack of scopes (module-level + per-type
// + per-method). Each scope contributes a small name → Symbol table.
struct Scope {
    std::unordered_map<std::string_view, Symbol*> names;
    Scope* parent = nullptr;
};

// Walker for type-resolution. Threads a Scope* through the AST visit.
class Resolver {
public:
    Resolver(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src) {
        Scope moduleScope;
        // Populate module-scope: every top-level type / alias by name.
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (!child->name.empty()) {
                    moduleScope.names[child->name] = child;
                }
            }
        }
        for (const auto& decl : src.topDecls) {
            if (!decl) continue;
            resolveTopDecl(*decl, &moduleScope);
        }
        // Slice S12: after every base-type reference is resolved,
        // walk each TypeSymbol's inheritance chain and report cycles.
        bool hadCycle = checkInheritanceCycles();
        // Slice S13: a concrete class must implement every inherited
        // abstract / interface member. Skipped when a cycle exists
        // (the chain walk wouldn't terminate cleanly).
        if (!hadCycle) checkAbstractImplementations();
        // Slice S15: hiding forbidden / override must target a virtual.
        if (!hadCycle) checkHidingAndOverride();
        // Slice S14: duplicate non-overloadable member names within a
        // single type.
        checkDuplicateMembers();
        // Slice S16: a static class may contain only static members.
        checkStaticClassMembers();
        // Slice S17: class-level modifier mutual-exclusions.
        checkClassModifierCombinations();
        // Slice S19: overloads must differ in signature.
        checkOverloadSignatures();
        // Slice S22: top-level types may only be public or internal.
        checkTopLevelVisibility();
        // Slice S23: record restrictions (§9.7).
        checkRecordRestrictions();
        // Slice S28: accessor visibility (§13.6).
        checkAccessorVisibility();
        // Slice S30: const member type must be primitive or string.
        checkConstTypes();
        // Slice S32: enum member restrictions (§10.5).
        checkEnumRestrictions();
        // Slice S33: method body-presence rules (§12.5 / §11.2).
        checkMethodBodies();
        // Slice S34: static members may not be virtual (§12.1).
        checkStaticNotVirtual();
        // Slice S35: `async abstract` is a compile error (§12.6).
        checkAsyncModifiers();
        // Slice S36: constructor chain-call placement (§12.8).
        checkConstructorChains();
        // Slice S37: static constructor takes no parameters (§22.7).
        checkStaticConstructorParams();
        // Slice S38: base/interface visibility (§13.7).
        checkBaseVisibility();
    }

    // Slice S38 (§13.7 / Appendix C.2): a class's base class and
    // implemented interfaces must be at least as visible as the class.
    // Effective top-level visibility defaults to `internal`. Restricted
    // to top-level types (nested-type effective visibility per §11.6 is
    // deferred). Bases resolving to a built-in (null declaration) are
    // effectively public and skipped.
    static int effVisRank(ast::Visibility v) {
        int r = visRank(v);
        return r == 0 ? 2 : r;  // None → internal
    }

    void checkBaseVisibility() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            int classRank = effVisRank(t->visibility);
            for (TypeSymbol* b : t->baseTypes) {
                if (!b) continue;  // built-in / unresolved — skip
                if (effVisRank(b->visibility) < classRank) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("base type `") + std::string(b->name) +
                        "` is less visible than `" + std::string(t->name) +
                        "` — a base class or implemented interface must be "
                        "at least as visible as the type (§13.7)");
                }
            }
        }
    }

    // Slice S37 (§22.7 / Appendix C.5): a static constructor takes no
    // parameters. (The no-access-modifier rule is a parser concern.)
    void checkStaticConstructorParams() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeStaticCtorParams(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeStaticCtorParams(nt->type);
                }
            }
        }
    }

    void checkTypeStaticCtorParams(TypeSymbol* t) {
        for (Symbol* m : t->members) {
            auto* cs = dynamic_cast<ConstructorSymbol*>(m);
            if (!cs || !cs->isStatic) continue;
            if (!cs->params.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("static constructor of `") +
                    std::string(t->name) +
                    "` may not declare parameters (§22.7)");
            }
        }
    }

    // Slice S36 (§12.8 / Appendix C.5): a constructor may contain at
    // most one `base(...)` / `this(...)` chain call, and it must be the
    // first statement (the chaining position).
    void checkConstructorChains() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeConstructorChains(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeConstructorChains(nt->type);
                }
            }
        }
    }

    // True if `s` is a constructor chain call: an expression statement
    // whose expression is a call on the bare identifier `this`/`base`.
    static bool isChainCall(const ast::Stmt* s) {
        auto* es = dynamic_cast<const ast::ExpressionStmt*>(s);
        if (!es || !es->expr) return false;
        auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
        if (!call || !call->callee) return false;
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(
            call->callee.get());
        return id && (id->name == "this" || id->name == "base");
    }

    void checkTypeConstructorChains(TypeSymbol* t) {
        for (Symbol* m : t->members) {
            auto* cs = dynamic_cast<ConstructorSymbol*>(m);
            if (!cs) continue;
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (!cd) continue;
            int chainCount = 0;
            for (std::size_t i = 0; i < cd->body.size(); ++i) {
                if (!cd->body[i] || !isChainCall(cd->body[i].get())) continue;
                ++chainCount;
                if (i != 0) {
                    ctx_.diagnostics().report(
                        Severity::Error, cd->body[i]->span.begin,
                        "a `base(...)` / `this(...)` constructor chain "
                        "call may appear only as the first statement "
                        "(§12.8)");
                }
            }
            if (chainCount > 1) {
                ctx_.diagnostics().report(
                    Severity::Error, cd->span.begin,
                    "a constructor may contain at most one `base(...)` "
                    "or `this(...)` chain call (§12.8)");
            }
        }
    }

    // Slice S35 (§12.6 / Appendix C.6): `async` does not combine with
    // `abstract` on a method. (An abstract async method declares a
    // Task / Task<T> return type without `async` instead.)
    void checkAsyncModifiers() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeAsyncModifiers(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeAsyncModifiers(nt->type);
                }
            }
        }
    }

    void checkTypeAsyncModifiers(TypeSymbol* t) {
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms) continue;
            if (hasMod(ms, "async") && hasMod(ms, "abstract")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("method `") + std::string(ms->name) +
                    "` may not be both `async` and `abstract`; declare a "
                    "`Task` / `Task<T>` return type without `async` "
                    "(§12.6)");
            }
        }
    }

    // Slice S34 (§12.1): "Fields, constructors, destructors, and static
    // members may not be virtual." A static method or property carrying
    // `virtual` / `abstract` / `override` is rejected.
    void checkStaticNotVirtual() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeStaticNotVirtual(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeStaticNotVirtual(nt->type);
                }
            }
        }
    }

    void checkTypeStaticNotVirtual(TypeSymbol* t) {
        for (Symbol* m : t->members) {
            auto* mem = dynamic_cast<MemberSymbol*>(m);
            if (!mem) continue;
            if (!dynamic_cast<MethodSymbol*>(m) &&
                !dynamic_cast<PropertySymbol*>(m)) {
                continue;
            }
            if (!hasMod(mem, "static")) continue;
            if (hasMod(mem, "virtual") || hasMod(mem, "abstract") ||
                hasMod(mem, "override")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("static member `") + std::string(mem->name) +
                    "` may not be virtual, abstract, or override (§12.1)");
            }
        }
    }

    // Slice S33: a method's body presence must match its kind.
    //  - In a class: a method marked `abstract` must have no body, and
    //    a non-abstract method must have one (§12.5).
    //  - In an interface: methods have no body (§11.2); a body is an
    //    error.
    // Records / enums: treated like classes (their methods are
    // concrete). Operators are always concrete and handled by the
    // parser's body requirement.
    void checkMethodBodies() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeMethodBodies(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeMethodBodies(nt->type);
                }
            }
        }
    }

    void checkTypeMethodBodies(TypeSymbol* t) {
        bool isInterface = t->kind == ast::TypeDeclKind::Interface;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms) continue;
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (!md) continue;
            bool noBody = md->bodyKind == ast::MethodBodyKind::Abstract;
            bool isAbstract = hasMod(ms, "abstract");
            if (isInterface) {
                if (!noBody) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("interface method `") +
                        std::string(ms->name) +
                        "` may not have a body (§11.2)");
                }
                continue;
            }
            if (isAbstract && !noBody) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("abstract method `") +
                    std::string(ms->name) +
                    "` may not have a body (§12.5)");
            } else if (!isAbstract && noBody) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("non-abstract method `") +
                    std::string(ms->name) +
                    "` must have a body (§12.5)");
            }
        }
    }

    // Slice S32 (§10.5): an enum may not declare instance fields,
    // constructors, nested types, or indexers, and its methods may not
    // be virtual / abstract / override / sealed override. Static
    // fields and static methods are permitted (§10.4).
    void checkEnumRestrictions() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t || t->kind != ast::TypeDeclKind::Enum) continue;
            for (Symbol* m : t->members) {
                auto bad = [&](std::string_view what) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("enum `") + std::string(t->name) +
                        "` may not declare " + std::string(what) +
                        " (§10.5)");
                };
                if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                    if (!hasMod(fs, "static")) bad("an instance field");
                    continue;
                }
                if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
                    if (!cs->isStatic) bad("a constructor");
                    continue;
                }
                if (dynamic_cast<NestedTypeSymbol*>(m)) {
                    bad("a nested type");
                    continue;
                }
                if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
                    if (ps->isIndexer) bad("an indexer");
                    continue;
                }
                if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                    if (hasMod(ms, "virtual") || hasMod(ms, "abstract") ||
                        hasMod(ms, "override")) {
                        ctx_.diagnostics().report(
                            Severity::Error, m->declLoc,
                            std::string("enum method `") +
                            std::string(ms->name) +
                            "` may not be virtual, abstract, or override "
                            "— enums dispatch statically (§10.5)");
                    }
                }
            }
        }
    }

    // Slice S30 (§22.12): a `const` declaration requires a primitive
    // value type or `string`. Object, named types, arrays, functions,
    // and unit are rejected. Only the explicit-ascription case is
    // checked here; type-inferred consts (and the compile-time-constant
    // requirement on the value) are deferred.
    void checkConstTypes() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeConsts(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeConsts(nt->type);
                }
            }
        }
    }

    void checkTypeConsts(TypeSymbol* t) {
        for (Symbol* m : t->members) {
            auto* cs = dynamic_cast<ConstMemberSymbol*>(m);
            if (!cs) continue;
            auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
            if (!cd || !cd->type) continue;  // inferred — deferred
            TypeRef* rt = ctx_.resolvedType(*cd->type);
            if (!rt) continue;
            bool ok = rt->kind == TypeRef::Kind::Primitive &&
                      rt->primitive != PrimitiveType::Object;
            if (!ok) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("const `") + std::string(cs->name) +
                    "` must have a primitive value type or `string` "
                    "(§22.12)");
            }
        }
    }

    // Slice S28 (§13.6): a property/indexer accessor's explicit
    // visibility must be more restrictive than the property's overall
    // visibility; an accessor that is *less* restrictive is a compile
    // error. Restrictiveness rank: public(3) > internal/protected(2) >
    // private(1). `internal` and `protected` are incomparable (same
    // rank), so we only flag a strictly-less-restrictive accessor.
    static int visRank(ast::Visibility v) {
        switch (v) {
            case ast::Visibility::Public: return 3;
            case ast::Visibility::Internal: return 2;
            case ast::Visibility::Protected: return 2;
            case ast::Visibility::Private: return 1;
            default: return 0;  // None — inherits; not checked
        }
    }

    void checkAccessorVisibility() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeAccessorVisibility(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeAccessorVisibility(nt->type);
                }
            }
        }
    }

    void checkTypeAccessorVisibility(TypeSymbol* t) {
        for (Symbol* m : t->members) {
            auto* ps = dynamic_cast<PropertySymbol*>(m);
            if (!ps) continue;
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd) continue;
            int propRank = visRank(pd->visibility);
            if (propRank == 0) continue;  // interface / no explicit vis
            auto checkOne = [&](ast::Visibility av, SourceLocation loc) {
                if (av == ast::Visibility::None) return;
                if (visRank(av) > propRank) {
                    ctx_.diagnostics().report(
                        Severity::Error, loc,
                        std::string("accessor of property `") +
                        std::string(ps->name) +
                        "` is less restrictive than the property's own "
                        "visibility (§13.6)");
                }
            };
            for (const auto& a : pd->accessors) {
                checkOne(a.visibility, pd->span.begin);
            }
            if (pd->hasAutoAccessorVis) {
                checkOne(pd->autoAccessorVis, pd->span.begin);
            }
        }
    }

    // Slice S24 (§2.3): generic arity of the always-available built-in
    // type names. kNotBuiltin means "not a built-in"; kBuiltinTaskLike
    // marks `Task`, which is valid both bare (`Task`) and unary
    // (`Task<T>`). `Object` is handled earlier as a primitive.
    static constexpr int kNotBuiltin = -1;
    static constexpr int kBuiltinTaskLike = -2;
    static int builtinTypeArity(std::string_view n) {
        if (n == "Type" || n == "IDisposable" || n == "IAsyncDisposable" ||
            n == "Subscription" || n == "CancellationToken") {
            return 0;
        }
        // Slice S25 (§31.4): language-built-in exception types, all
        // nullary, available without import.
        if (n == "Exception" || n == "ArgumentException" ||
            n == "ArgumentNullException" || n == "ArithmeticException" ||
            n == "OverflowException" || n == "DivideByZeroException" ||
            n == "InvalidOperationException" ||
            n == "NotImplementedException" ||
            n == "NullReferenceException" ||
            n == "IndexOutOfRangeException" ||
            n == "MatchFailureException" ||
            n == "OperationCanceledException" ||
            n == "TypeInitializationException" ||
            n == "FormatException" || n == "StackOverflowException") {
            return 0;
        }
        if (n == "Task") return kBuiltinTaskLike;
        if (n == "Array" || n == "OwnedArray" || n == "OwnedTask" ||
            n == "IEnumerable" || n == "IEnumerator" ||
            n == "IAsyncEnumerable" || n == "IAsyncEnumerator" ||
            n == "OwnedEnumerable" || n == "OwnedEnumerator" ||
            n == "OwnedAsyncEnumerable" || n == "OwnedAsyncEnumerator" ||
            n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
            n == "ICollection" || n == "IList" ||
            n == "IOwnedCollection" || n == "IOwnedList" ||
            n == "IEvent" || n == "IAsyncEvent") {
            return 1;
        }
        return kNotBuiltin;
    }

    // Slice S23 (§9.7): a record may not have a destructor, an `owned`
    // field, an inline-array field whose slots carry the `owned`
    // marker, or implement `IDisposable`. Records have copy semantics
    // only and never own a resource.
    void checkRecordRestrictions() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t || t->kind != ast::TypeDeclKind::Record) continue;
            for (Symbol* m : t->members) {
                if (dynamic_cast<DestructorSymbol*>(m)) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("record `") + std::string(t->name) +
                        "` may not declare a destructor (§9.7)");
                    continue;
                }
                if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                    if (hasMod(fs, "owned")) {
                        ctx_.diagnostics().report(
                            Severity::Error, m->declLoc,
                            std::string("record `") + std::string(t->name) +
                            "` may not have an `owned` field `" +
                            std::string(fs->name) + "` (§9.7)");
                        continue;
                    }
                    // Inline-array field with owned slots.
                    if (auto* fd =
                            dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                        if (fd->type) {
                            auto* rt = ctx_.resolvedType(*fd->type);
                            if (rt &&
                                rt->kind == TypeRef::Kind::InlineArray &&
                                rt->arrayOwned) {
                                ctx_.diagnostics().report(
                                    Severity::Error, m->declLoc,
                                    std::string("record `") +
                                    std::string(t->name) +
                                    "` may not have an owning inline-array "
                                    "field `" + std::string(fs->name) +
                                    "` (§9.7)");
                            }
                        }
                    }
                }
            }
            // IDisposable implementation: name-based check on the AST
            // base-type list (the built-in IDisposable has no user
            // TypeSymbol to resolve against).
            if (auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl)) {
                for (const auto& bt : td->baseTypes) {
                    if (bt && bt->name && !bt->name->parts.empty() &&
                        bt->name->parts.back() == "IDisposable") {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            std::string("record `") + std::string(t->name) +
                            "` may not implement `IDisposable` (§9.7)");
                    }
                }
            }
        }
    }

    // Slice S22 (§13.2): a top-level type may be `public` or `internal`
    // (the latter is also the default when no visibility keyword is
    // written). `protected` and `private` are rejected — there is no
    // enclosing class for `protected` and no file-scope concept for
    // `private`.
    void checkTopLevelVisibility() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            if (t->visibility == ast::Visibility::Private ||
                t->visibility == ast::Visibility::Protected) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("top-level type `") + std::string(t->name) +
                    "` may only be `public` or `internal` (§13.2)");
            }
        }
    }

    // Slice S19: structural equality of two resolved TypeRefs. Returns
    // false (conservatively) when either is null or an Error kind — the
    // caller skips the overload-equality check rather than risk a false
    // collision on a not-yet-resolvable type.
    static bool typeRefEqual(const TypeRef* a, const TypeRef* b) {
        if (!a || !b) return false;
        if (a->kind != b->kind) return false;
        if (a->nullable != b->nullable) return false;
        switch (a->kind) {
            case TypeRef::Kind::Error:
                return false;
            case TypeRef::Kind::Unit:
                return true;
            case TypeRef::Kind::Primitive:
                return a->primitive == b->primitive;
            case TypeRef::Kind::Named: {
                if (a->declaration != b->declaration) return false;
                if (a->genericArgs.size() != b->genericArgs.size())
                    return false;
                for (std::size_t i = 0; i < a->genericArgs.size(); ++i) {
                    if (!typeRefEqual(a->genericArgs[i], b->genericArgs[i]))
                        return false;
                }
                return true;
            }
            case TypeRef::Kind::InlineArray:
                return a->arrayOwned == b->arrayOwned &&
                       typeRefEqual(a->arrayElement, b->arrayElement);
            case TypeRef::Kind::Function: {
                if (a->funcParams.size() != b->funcParams.size())
                    return false;
                for (std::size_t i = 0; i < a->funcParams.size(); ++i) {
                    if (a->funcParams[i].modifier != b->funcParams[i].modifier)
                        return false;
                    if (!typeRefEqual(a->funcParams[i].type,
                                      b->funcParams[i].type))
                        return false;
                }
                return a->funcReturnOwned == b->funcReturnOwned &&
                       typeRefEqual(a->funcReturn, b->funcReturn);
            }
        }
        return false;
    }

    // Slice S19: the overload-distinguishing form of a parameter
    // modifier (§22.6). `owned` on an ordinary parameter does NOT
    // distinguish overloads — it normalizes to the plain value form —
    // so two overloads differing only by ordinary `owned` collide.
    // `out owned` IS distinct from `out`.
    static std::string_view overloadModifier(std::string_view mod) {
        if (mod == "owned" || mod.empty()) return "value";
        if (mod == "out owned") return "out owned";
        if (mod == "out") return "out";
        if (mod == "ref") return "ref";
        return mod;
    }

    // Slice S19 (§22.6): within a type, two same-named methods (or two
    // constructors) may not have signatures that are indistinguishable
    // under overload resolution — same parameter count, the same
    // overload-modifier on each parameter, and structurally equal
    // parameter types. Conservatively skips a pair when any parameter
    // type fails to resolve.
    void checkOverloadSignatures() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeOverloads(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeOverloads(nt->type);
                }
            }
        }
    }

    void checkTypeOverloads(TypeSymbol* t) {
        using Overload = std::pair<const std::vector<Symbol*>*, Symbol*>;
        // Methods grouped by name; constructors grouped separately
        // (they overload as a single name-less family).
        std::unordered_map<std::string_view, std::vector<Overload>> methods;
        std::vector<Overload> ctors;
        for (Symbol* m : t->members) {
            if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                methods[ms->name].push_back({&ms->params, m});
            } else if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
                if (!cs->isStatic) ctors.push_back({&cs->params, m});
            }
        }
        auto checkGroup = [&](std::vector<Overload>& overloads,
                              const std::string& label) {
            for (std::size_t i = 0; i < overloads.size(); ++i) {
                for (std::size_t j = i + 1; j < overloads.size(); ++j) {
                    if (signaturesIndistinguishable(*overloads[i].first,
                                                    *overloads[j].first)) {
                        ctx_.diagnostics().report(
                            Severity::Error, overloads[j].second->declLoc,
                            std::string("overload of ") + label +
                            " is indistinguishable from another overload "
                            "— overloads must differ in parameter count, "
                            "types, or `ref`/`out` modifiers; ordinary "
                            "`owned` does not distinguish (§22.6)");
                    }
                }
            }
        };
        for (auto& [name, overloads] : methods) {
            checkGroup(overloads, std::string("`") + std::string(name) + "`");
        }
        checkGroup(ctors, "constructor");
    }

    bool signaturesIndistinguishable(const std::vector<Symbol*>& a,
                                     const std::vector<Symbol*>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            auto* pa = dynamic_cast<ParameterSymbol*>(a[i]);
            auto* pb = dynamic_cast<ParameterSymbol*>(b[i]);
            if (!pa || !pb) return false;
            if (overloadModifier(pa->modifier) !=
                overloadModifier(pb->modifier)) {
                return false;
            }
            TypeRef* ta = pa->declType ? ctx_.resolvedType(*pa->declType)
                                       : nullptr;
            TypeRef* tb = pb->declType ? ctx_.resolvedType(*pb->declType)
                                       : nullptr;
            if (!typeRefEqual(ta, tb)) return false;
        }
        return true;
    }

    // Slice S17 (§11.7): `abstract` and `static` are mutually
    // exclusive; `sealed` and `static` are mutually exclusive.
    // `sealed abstract` is permitted (a closed abstract class).
    void checkClassModifierCombinations() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t || t->kind != ast::TypeDeclKind::Class) continue;
            bool isStatic = false, isAbstract = false, isSealed = false;
            for (auto mod : t->modifiers) {
                if (mod == "static") isStatic = true;
                else if (mod == "abstract") isAbstract = true;
                else if (mod == "sealed") isSealed = true;
            }
            if (isStatic && isAbstract) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("class `") + std::string(t->name) +
                    "` may not be both `abstract` and `static` "
                    "(mutually exclusive — §11.7)");
            }
            if (isStatic && isSealed) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("class `") + std::string(t->name) +
                    "` may not be both `sealed` and `static` "
                    "(mutually exclusive — §11.7)");
            }
        }
    }

    // Slice S16 (§11.7): a class declared `static` may contain only
    // static members — no instance fields, methods, properties,
    // indexers, instance constructors, destructors, or owned fields.
    // A static constructor, static members, class-level consts (which
    // are implicitly static), and nested types are permitted.
    void checkStaticClassMembers() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t || t->kind != ast::TypeDeclKind::Class) continue;
            bool isStatic = false;
            for (auto mod : t->modifiers) {
                if (mod == "static") { isStatic = true; break; }
            }
            if (!isStatic) continue;
            for (Symbol* m : t->members) {
                checkStaticClassMember(t, m);
            }
        }
    }

    void checkStaticClassMember(TypeSymbol* t, Symbol* m) {
        auto reportInstance = [&](std::string_view kind,
                                  std::string_view name) {
            ctx_.diagnostics().report(
                Severity::Error,
                m->declLoc,
                std::string("static class `") + std::string(t->name) +
                "` may not declare instance " + std::string(kind) +
                " `" + std::string(name) + "` (§11.7 — a static class "
                "contains only static members)");
        };
        if (auto* dd = dynamic_cast<DestructorSymbol*>(m)) {
            (void)dd;
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("static class `") + std::string(t->name) +
                "` may not declare a destructor (§11.7)");
            return;
        }
        if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
            if (!cs->isStatic) reportInstance("constructor", t->name);
            return;
        }
        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
            if (!hasMod(fs, "static")) {
                reportInstance("field", fs->name);
            } else if (hasMod(fs, "owned")) {
                // §11.7: a static class may not have owned fields at all.
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("static class `") + std::string(t->name) +
                    "` may not declare an owned field `" +
                    std::string(fs->name) + "` (§11.7)");
            }
            return;
        }
        if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            if (!hasMod(ms, "static")) reportInstance("method", ms->name);
            return;
        }
        if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (!hasMod(ps, "static")) {
                reportInstance(ps->isIndexer ? "indexer" : "property",
                               ps->name);
            }
            return;
        }
        if (auto* es = dynamic_cast<EventSymbol*>(m)) {
            if (!hasMod(es, "static")) reportInstance("event", es->name);
            return;
        }
        // ConstMemberSymbol (implicitly static), NestedTypeSymbol,
        // EnumConstantSymbol — permitted.
    }

    // Slice S14: within one type, two members that aren't both
    // overloadable cannot share a name. Methods and constructors are
    // overloadable (same name, different signature is fine); fields,
    // properties, events, constants, and nested types are not. A
    // collision between a non-overloadable member and ANY other member
    // of the same name is an error.
    void checkDuplicateMembers() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            checkTypeDuplicates(t);
        }
    }

    static bool isOverloadable(Symbol* m) {
        return dynamic_cast<MethodSymbol*>(m) != nullptr ||
               dynamic_cast<ConstructorSymbol*>(m) != nullptr;
    }

    void checkTypeDuplicates(TypeSymbol* t) {
        // Map name → the first member seen with that name. When a later
        // member collides and at least one side is non-overloadable,
        // diagnose at the later member.
        std::unordered_map<std::string_view, Symbol*> seen;
        for (Symbol* m : t->members) {
            if (!m || m->name.empty()) continue;
            // Constructors share the empty/synthetic name in some
            // designs, but here they carry the type name or are
            // overloadable anyway — skip diagnosing constructor-vs
            // collisions.
            auto it = seen.find(m->name);
            if (it == seen.end()) {
                seen[m->name] = m;
                continue;
            }
            Symbol* prior = it->second;
            // If both are overloadable, it's a legal overload pair.
            if (isOverloadable(m) && isOverloadable(prior)) {
                continue;
            }
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("duplicate member `") + std::string(m->name) +
                "` in type `" + std::string(t->name) +
                "` (a non-overloadable member cannot share a name with "
                "another member of the same type; §11.3 lists the member "
                "kinds — uniqueness is a well-formedness requirement)");
        }
        // Recurse into nested types.
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeDuplicates(nt->type);
            }
        }
    }

    // Slice S12: DFS over baseTypes; if we revisit a node already in
    // the current path, report a cycle anchored at the offending
    // TypeSymbol. Each cycle is reported once (at its smallest entry).
    // Returns true if any cycle was found.
    bool checkInheritanceCycles() {
        if (!ctx_.module()) return false;
        std::unordered_set<TypeSymbol*> reported;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            std::unordered_set<TypeSymbol*> onStack;
            std::unordered_set<TypeSymbol*> done;
            std::vector<TypeSymbol*> path;
            checkCycleFrom(t, onStack, done, path, reported);
        }
        return !reported.empty();
    }

    // Slice S13: is this method member abstract? A method is abstract
    // when its parent is an interface (interface methods are implicitly
    // abstract), when it carries the `abstract` modifier, or when its
    // MethodDecl has an Abstract body.
    static bool hasMod(MemberSymbol* m, std::string_view want) {
        for (auto mod : m->modifiers) {
            if (mod == want) return true;
        }
        return false;
    }

    // Slice S15: a base member is overridable when it is `virtual`,
    // `abstract`, or `override` — but not `sealed override` (which
    // seals it against further overriding, §12.4).
    static bool baseMemberOverridable(MemberSymbol* m) {
        if (hasMod(m, "virtual") || hasMod(m, "abstract")) return true;
        if (hasMod(m, "override")) return !hasMod(m, "sealed");
        return false;
    }

    // Slice S20 (§12.1): an override must match the base member's
    // visibility, return type, parameter types, and parameter modifiers
    // exactly. Parameter modifiers that must match: ref, out, owned.
    // Return modifier that must match: owned. Only methods are handled
    // here (property/indexer override-match is a later slice). When the
    // base member of the same name is itself an overload set, this
    // matches against the single representative S15 recorded — refining
    // to overload-aware matching awaits the overload-resolution slice.
    // §12.1 / §14: when a derived class overrides a method from a generic
    // base class (e.g. `class Box : Container<int>`), the base method's
    // type references use the generic param (T). Apply the substitution
    // T→int before comparing against the override's concrete types.
    // Returns the substituted TypeRef if `tr` is a generic param that
    // appears in `genericSubst`, otherwise returns `tr` unchanged.
    TypeRef* applyGenericSubst(
            TypeRef* tr,
            const std::unordered_map<std::string_view, TypeRef*>& genericSubst) {
        if (!tr) return tr;
        if (tr->kind == TypeRef::Kind::Named && !tr->declaration) {
            // Unresolved named type — might be a generic param.
            if (tr->astNode && tr->astNode->name &&
                tr->astNode->name->parts.size() == 1) {
                auto it = genericSubst.find(tr->astNode->name->parts[0]);
                if (it != genericSubst.end()) return it->second;
            }
        }
        return tr;
    }

    void checkOverrideSignatureMatch(MemberSymbol* derivedSym,
                                     MemberSymbol* baseSym) {
        auto* derived = dynamic_cast<MethodSymbol*>(derivedSym);
        auto* base = dynamic_cast<MethodSymbol*>(baseSym);
        if (!derived || !base) return;  // not a method pair — skip.
        auto err = [&](const std::string& what) {
            ctx_.diagnostics().report(
                Severity::Error, derived->declLoc,
                std::string("override of `") + std::string(derived->name) +
                "` does not match the base member's " + what + " (§12.1)");
        };

        // Build generic substitution map: base class's T → concrete arg.
        // Example: Container<T> base, IntBox : Container<int> derived →
        // {T → int}. Look up the base type's TypeRef from the derived
        // class's baseTypeRefs (which store the resolved generic args).
        std::unordered_map<std::string_view, TypeRef*> genericSubst;
        TypeSymbol* derivedClass = derived->parent;
        TypeSymbol* baseClass = base->parent;
        if (derivedClass && baseClass) {
            for (std::size_t bi = 0; bi < derivedClass->baseTypes.size(); ++bi) {
                if (derivedClass->baseTypes[bi] != baseClass) continue;
                TypeRef* baseRef = (bi < derivedClass->baseTypeRefs.size())
                                   ? derivedClass->baseTypeRefs[bi] : nullptr;
                if (!baseRef) break;
                // Match base class generic params to the concrete args.
                for (std::size_t gi = 0; gi < baseClass->genericParams.size() &&
                     gi < baseRef->genericArgs.size(); ++gi) {
                    if (auto* gp = dynamic_cast<GenericParamSymbol*>(
                                      baseClass->genericParams[gi])) {
                        genericSubst[gp->name] = baseRef->genericArgs[gi];
                    }
                }
                break;
            }
        }

        // Visibility must match exactly.
        if (derived->visibility != base->visibility) {
            err("visibility");
        }
        auto* dDecl = dynamic_cast<const ast::MethodDecl*>(derived->decl);
        auto* bDecl = dynamic_cast<const ast::MethodDecl*>(base->decl);
        if (dDecl && bDecl) {
            // Return type (null == unit) and owned-return modifier.
            TypeRef* dRet = dDecl->returnType
                ? ctx_.resolvedType(*dDecl->returnType) : nullptr;
            TypeRef* bRet = bDecl->returnType
                ? ctx_.resolvedType(*bDecl->returnType) : nullptr;
            // Apply substitution to base return type (T → int, etc.).
            TypeRef* bRetSubst = applyGenericSubst(bRet, genericSubst);
            bool bothNullReturn = !dDecl->returnType && !bDecl->returnType;
            if (!bothNullReturn && !typeRefEqual(dRet, bRetSubst)) {
                err("return type");
            }
            if (dDecl->returnOwned != bDecl->returnOwned) {
                err("`owned` return modifier");
            }
        }
        // Parameters: count, exact modifier, exact type.
        if (derived->params.size() != base->params.size()) {
            err("parameter count");
            return;
        }
        for (std::size_t i = 0; i < derived->params.size(); ++i) {
            auto* dp = dynamic_cast<ParameterSymbol*>(derived->params[i]);
            auto* bp = dynamic_cast<ParameterSymbol*>(base->params[i]);
            if (!dp || !bp) continue;
            if (dp->modifier != bp->modifier) {
                err("parameter modifiers");
                return;
            }
            TypeRef* dt = dp->declType ? ctx_.resolvedType(*dp->declType)
                                       : nullptr;
            TypeRef* bt = bp->declType ? ctx_.resolvedType(*bp->declType)
                                       : nullptr;
            // Apply substitution to base parameter type.
            TypeRef* btSubst = applyGenericSubst(bt, genericSubst);
            if (dt && btSubst && !typeRefEqual(dt, btSubst)) {
                err("parameter types");
                return;
            }
        }
    }

    // Slice S15: §12.2 forbids a derived class from reusing a base
    // CLASS member's name except to override a virtual/abstract base
    // member (§12.1). Interface members are excluded — re-declaring an
    // interface member is implementation (matching), not hiding.
    void checkHidingAndOverride() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t || t->kind != ast::TypeDeclKind::Class) continue;
            // Gather base CLASS members by name (transitive).
            std::unordered_map<std::string_view, MemberSymbol*> baseMembers;
            std::vector<TypeSymbol*> frontier;
            for (TypeSymbol* b : t->baseTypes) {
                if (b && b->kind == ast::TypeDeclKind::Class) {
                    frontier.push_back(b);
                }
            }
            std::unordered_set<TypeSymbol*> seen;
            std::size_t i = 0;
            while (i < frontier.size()) {
                TypeSymbol* cur = frontier[i++];
                if (!seen.insert(cur).second) continue;
                for (Symbol* m : cur->members) {
                    // Constructors and destructors are not inherited, so
                    // they never participate in hiding/override.
                    if (dynamic_cast<ConstructorSymbol*>(m) ||
                        dynamic_cast<DestructorSymbol*>(m)) {
                        continue;
                    }
                    if (auto* ms = dynamic_cast<MemberSymbol*>(m)) {
                        // First (most-derived) base wins on collision.
                        baseMembers.emplace(ms->name, ms);
                    }
                }
                for (TypeSymbol* b : cur->baseTypes) {
                    if (b && b->kind == ast::TypeDeclKind::Class) {
                        frontier.push_back(b);
                    }
                }
            }
            if (baseMembers.empty()) continue;
            for (Symbol* m : t->members) {
                if (dynamic_cast<ConstructorSymbol*>(m) ||
                    dynamic_cast<DestructorSymbol*>(m)) {
                    continue;
                }
                auto* ms = dynamic_cast<MemberSymbol*>(m);
                if (!ms || ms->name.empty()) continue;
                auto it = baseMembers.find(ms->name);
                if (it == baseMembers.end()) continue;
                MemberSymbol* base = it->second;
                bool isOverride = hasMod(ms, "override");
                if (isOverride) {
                    if (!baseMemberOverridable(base)) {
                        ctx_.diagnostics().report(
                            Severity::Error, ms->declLoc,
                            std::string("`") + std::string(ms->name) +
                            "` is declared `override` but the base "
                            "member it would override is not `virtual`, "
                            "`abstract`, or `override` (§12.1)");
                    } else {
                        // Slice S20: a legal override must match the
                        // base member's signature exactly.
                        checkOverrideSignatureMatch(ms, base);
                    }
                } else {
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        std::string("member `") + std::string(ms->name) +
                        "` hides an inherited member of the same name; "
                        "hiding is forbidden — use `override` for a "
                        "virtual base member, or choose a different "
                        "name (§12.2)");
                }
            }
        }
    }

    bool methodIsAbstract(MethodSymbol* ms) {
        if (ms->parent &&
            ms->parent->kind == ast::TypeDeclKind::Interface) {
            return true;
        }
        for (auto mod : ms->modifiers) {
            if (mod == "abstract") return true;
        }
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (md->bodyKind == ast::MethodBodyKind::Abstract) return true;
        }
        return false;
    }

    // Slice S13: walk a concrete class's inheritance chain; every
    // abstract method name must be matched by a concrete method
    // somewhere in the chain. Name-based (overload-aware matching is a
    // later slice).
    void checkAbstractImplementations() {
        if (!ctx_.module()) return;
        for (Symbol* child : ctx_.module()->children) {
            auto* t = dynamic_cast<TypeSymbol*>(child);
            if (!t) continue;
            if (t->kind != ast::TypeDeclKind::Class) continue;
            bool isAbstract = false;
            for (auto mod : t->modifiers) {
                if (mod == "abstract") { isAbstract = true; break; }
            }
            if (isAbstract) continue;  // abstract classes need not impl.

            std::unordered_set<std::string_view> abstractNames;
            std::unordered_set<std::string_view> concreteNames;
            // BFS the chain.
            std::vector<TypeSymbol*> frontier{t};
            std::unordered_set<TypeSymbol*> seen{t};
            std::size_t i = 0;
            while (i < frontier.size()) {
                TypeSymbol* cur = frontier[i++];
                for (Symbol* m : cur->members) {
                    if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                        if (methodIsAbstract(ms))
                            abstractNames.insert(ms->name);
                        else
                            concreteNames.insert(ms->name);
                    }
                }
                for (TypeSymbol* b : cur->baseTypes) {
                    if (b && !seen.count(b)) {
                        seen.insert(b);
                        frontier.push_back(b);
                    }
                }
            }
            for (auto name : abstractNames) {
                if (!concreteNames.count(name)) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("concrete class `") +
                        std::string(t->name) +
                        "` does not implement inherited abstract member `" +
                        std::string(name) + "` (§12.5)");
                }
            }
        }
    }

    // Per §12: inheritance must be well-founded (no cycles).  Diamond
    // inheritance (shared base through multiple paths) is legal for interfaces
    // — each base is visited once per DFS path, not globally, so a shared
    // ancestor is not a cycle.  `onStack` = nodes currently being explored
    // (detecting true back-edges); `done` = fully-explored nodes that need
    // not be re-entered.
    bool checkCycleFrom(
        TypeSymbol* t,
        std::unordered_set<TypeSymbol*>& onStack,
        std::unordered_set<TypeSymbol*>& done,
        std::vector<TypeSymbol*>& path,
        std::unordered_set<TypeSymbol*>& reported) {
        if (done.count(t)) return false;  // already fully explored, no cycle
        if (onStack.count(t)) {
            // Back-edge → actual cycle.
            if (!reported.count(t)) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("circular inheritance detected at `") +
                    std::string(t->name) +
                    "` — base-type chain re-enters this type "
                    "(circular inheritance is ill-formed: the dispatch "
                    "model of §12 presupposes a well-founded base chain)");
                reported.insert(t);
            }
            return true;
        }
        onStack.insert(t);
        path.push_back(t);
        for (TypeSymbol* b : t->baseTypes) {
            if (b) checkCycleFrom(b, onStack, done, path, reported);
        }
        path.pop_back();
        onStack.erase(t);
        done.insert(t);
        return false;
    }

private:
    SemaContext& ctx_;

    void resolveTopDecl(const ast::TopDecl& decl, Scope* parent) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(&decl)) {
            resolveTypeDecl(*td, parent);
        } else if (auto* alias = dynamic_cast<const ast::TypeAlias*>(&decl)) {
            Scope aliasScope;
            aliasScope.parent = parent;
            for (const auto& g : alias->genericParams) {
                // Register a temporary symbol for lookup; we don't need to
                // create a TypeSymbol stand-in because the alias body only
                // ever references the name.
                aliasScope.names[g.name] = nullptr; // marker for "is generic"
            }
            if (alias->aliasedType) {
                resolveType(*alias->aliasedType, &aliasScope);
            }
            for (const auto& gc : alias->genericConstraints) {
                for (const auto& item : gc.items) {
                    if (item.typeArg) resolveType(*item.typeArg, &aliasScope);
                }
            }
        } else if (auto* ext =
                       dynamic_cast<const ast::ExtensionDecl*>(&decl)) {
            Scope extScope;
            extScope.parent = parent;
            if (ext->targetType) resolveType(*ext->targetType, &extScope);
            for (const auto& gc : ext->genericConstraints) {
                for (const auto& item : gc.items) {
                    if (item.typeArg) resolveType(*item.typeArg, &extScope);
                }
            }
            for (const auto& m : ext->members) {
                if (m) resolveMember(*m, &extScope);
            }
        } else if (auto* xb =
                       dynamic_cast<const ast::ExternBlock*>(&decl)) {
            for (const auto& d : xb->methods) {
                for (const auto& p : d.params) {
                    if (p.type) resolveType(*p.type, parent);
                }
                if (d.returnType) resolveType(*d.returnType, parent);
            }
        }
    }

    void resolveTypeDecl(const ast::TypeDecl& td, Scope* parent) {
        Scope typeScope;
        typeScope.parent = parent;
        // Register the type's generic params + any nested types so members
        // can reference them.
        for (const auto& g : td.genericParams) {
            typeScope.names[g.name] = nullptr;  // marker for generic-param
        }
        for (const auto& m : td.members) {
            if (auto* nt =
                    dynamic_cast<const ast::NestedTypeMember*>(m.get())) {
                if (nt->decl) {
                    // Look up the corresponding TypeSymbol by name in the
                    // outer type's members — slice 1 stored them already.
                    // For now we just register the name as visible.
                    typeScope.names[nt->decl->name] = nullptr;
                }
            }
        }
        // Base types. Slice S5: cache the resolved TypeSymbols on the
        // declaring TypeSymbol so member lookup can walk them.
        TypeSymbol* tsym = nullptr;
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                    if (ts->decl == &td) { tsym = ts; break; }
                }
            }
        }
        // Slice S10: a class has at most one class-kind base; the
        // rest must be interfaces. Records and enums don't take
        // base classes (we don't enforce here — that's a separate
        // syntactic/structural check).
        std::size_t classBaseCount = 0;
        for (const auto& bt : td.baseTypes) {
            if (!bt) continue;
            TypeRef* ref = resolveType(*bt, &typeScope);
            if (tsym && ref && ref->kind == TypeRef::Kind::Named &&
                ref->declaration) {
                tsym->baseTypes.push_back(ref->declaration);
                tsym->baseTypeRefs.push_back(ref);  // preserve generic args
                TypeSymbol* base = ref->declaration;
                // Slice S9: a sealed class cannot be inherited from.
                if (base->kind == ast::TypeDeclKind::Class) {
                    ++classBaseCount;
                    if (td.kind == ast::TypeDeclKind::Class &&
                        classBaseCount > 1) {
                        ctx_.diagnostics().report(
                            Severity::Error, bt->span.begin,
                            std::string("class `") + std::string(td.name) +
                            "` already has a base class; only the "
                            "first base may be a class (single-"
                            "inheritance — §11.1)");
                    }
                    bool sealed = false;
                    for (auto mod : base->modifiers) {
                        if (mod == "sealed") { sealed = true; break; }
                    }
                    if (sealed) {
                        ctx_.diagnostics().report(
                            Severity::Error, bt->span.begin,
                            std::string("cannot inherit from sealed class `") +
                            std::string(base->name) + "` (§12.4)");
                    }
                }
            }
        }
        // Generic constraints.
        for (const auto& gc : td.genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &typeScope);
            }
        }
        // Positional fields (records).
        for (const auto& pf : td.positionalFields) {
            if (pf.type) resolveType(*pf.type, &typeScope);
        }
        // SerializedClassClause polymorphic case types.
        if (td.serializedClass) {
            for (const auto& pc : td.serializedClass->polyCases) {
                if (pc.type) resolveType(*pc.type, &typeScope);
            }
        }
        // Members.
        for (const auto& m : td.members) {
            if (m) resolveMember(*m, &typeScope);
        }
        // Enum constants don't carry types syntactically (their `:= value`
        // is a Const expression; types come from the underlying-int).
    }

    void resolveMember(const ast::Member& m, Scope* parent) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
            if (fd->type) resolveType(*fd->type, parent);
            // Issue #163: resolve types embedded in the initializer
            // expression (e.g. `new T()`), so a class-typed field
            // initializer is fully typed — mirrors what ctor/method
            // bodies get via resolveTypesInBody below.
            if (fd->initializer) resolveTypesInExpr(*fd->initializer, parent);
        } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
            Scope methodScope;
            methodScope.parent = parent;
            for (const auto& g : md->genericParams) {
                methodScope.names[g.name] = nullptr;
            }
            for (const auto& p : md->params) {
                if (p.type) resolveType(*p.type, &methodScope);
            }
            if (md->returnType) resolveType(*md->returnType, &methodScope);
            for (const auto& gc : md->genericConstraints) {
                for (const auto& item : gc.items) {
                    if (item.typeArg) resolveType(*item.typeArg, &methodScope);
                }
            }
            // Slice 3d: also resolve type ascriptions inside the method
            // body (LocalDecl `: Type`, foreach iteration type, …) so
            // ExprTypecheck can pick them up to type identifier uses.
            if (md->bodyKind == ast::MethodBodyKind::Block) {
                resolveTypesInBody(md->stmts, &methodScope);
            } else if (md->bodyKind == ast::MethodBodyKind::Expression &&
                       md->exprBody) {
                // Resolve types embedded in an expression body too (e.g.
                // `=> new T()`), mirroring block bodies — otherwise a
                // factory method's `new T()` has no resolved type.
                resolveTypesInExpr(*md->exprBody, &methodScope);
            }
        } else if (auto* od =
                       dynamic_cast<const ast::OperatorDecl*>(&m)) {
            Scope methodScope;
            methodScope.parent = parent;
            for (const auto& p : od->params) {
                if (p.type) resolveType(*p.type, &methodScope);
            }
            if (od->returnType) resolveType(*od->returnType, &methodScope);
            if (od->bodyKind == ast::MethodBodyKind::Block) {
                resolveTypesInBody(od->stmts, &methodScope);
            } else if (od->bodyKind == ast::MethodBodyKind::Expression &&
                       od->exprBody) {
                resolveTypesInExpr(*od->exprBody, &methodScope);
            }
        } else if (auto* cd =
                       dynamic_cast<const ast::ConstructorDecl*>(&m)) {
            Scope ctorScope;
            ctorScope.parent = parent;
            for (const auto& p : cd->params) {
                if (p.type) resolveType(*p.type, &ctorScope);
            }
            resolveTypesInBody(cd->body, &ctorScope);
        } else if (auto* dd =
                       dynamic_cast<const ast::DestructorDecl*>(&m)) {
            resolveTypesInBody(dd->body, parent);
        } else if (auto* pd =
                       dynamic_cast<const ast::PropertyDecl*>(&m)) {
            if (pd->type) resolveType(*pd->type, parent);
            for (const auto& p : pd->indexerParams) {
                if (p.type) resolveType(*p.type, parent);
            }
        } else if (auto* ed = dynamic_cast<const ast::EventDecl*>(&m)) {
            if (ed->type) resolveType(*ed->type, parent);
        } else if (auto* cd2 = dynamic_cast<const ast::ConstDecl*>(&m)) {
            if (cd2->type) resolveType(*cd2->type, parent);
        } else if (auto* nt =
                       dynamic_cast<const ast::NestedTypeMember*>(&m)) {
            if (nt->decl) resolveTypeDecl(*nt->decl, parent);
        }
    }

    Symbol* lookupName(std::string_view name, Scope* scope) {
        for (Scope* s = scope; s; s = s->parent) {
            auto it = s->names.find(name);
            if (it != s->names.end()) return it->second;
        }
        return nullptr;
    }

    // Slice 3d: walk a method/ctor body and resolve every type
    // ascription on LocalDecl / catch arm. Recurses through control-
    // flow constructs. Locals themselves are not registered here (that
    // happens later in ExprTypecheck); we only need their `: Type`
    // ascriptions resolved so identifier-on-local typing works.
    //
    // Slice 3f: also resolves types embedded in expressions —
    // NewExpr::type, TypeTestExpr::testType, TypeOfExpr::queriedType,
    // DefaultExpr::type, lambda return/param types — so ExprTypecheck
    // can type those expressions.
    void resolveTypesInBody(
        const std::vector<std::unique_ptr<ast::Stmt>>& body, Scope* scope);
    void resolveTypesInStmt(const ast::Stmt& s, Scope* scope);
    void resolveTypesInExpr(const ast::Expr& e, Scope* scope);

    TypeRef* resolveType(const ast::Type& t, Scope* scope) {
        auto* ref = ctx_.makeTypeRef();
        ref->astNode = &t;
        ref->nullable = t.nullable;
        switch (t.kind) {
            case ast::TypeKind::Unit:
                ref->kind = TypeRef::Kind::Unit;
                break;
            case ast::TypeKind::InlineArray:
                ref->kind = TypeRef::Kind::InlineArray;
                ref->arraySize = t.arraySize.get();
                ref->arrayOwned = t.arrayOwned;
                if (t.arrayElement) {
                    ref->arrayElement = resolveType(*t.arrayElement, scope);
                }
                break;
            case ast::TypeKind::Function:
                ref->kind = TypeRef::Kind::Function;
                ref->funcReturnOwned = t.funcReturnOwned;
                for (const auto& p : t.funcParams) {
                    TypeRef::FuncParam fp;
                    fp.modifier = p.modifier;
                    fp.name = p.name;
                    if (p.type) fp.type = resolveType(*p.type, scope);
                    ref->funcParams.push_back(std::move(fp));
                }
                if (t.funcReturn) {
                    ref->funcReturn = resolveType(*t.funcReturn, scope);
                }
                break;
            case ast::TypeKind::Named:
                resolveNamed(t, scope, *ref);
                break;
        }
        ctx_.setResolvedType(&t, ref);
        return ref;
    }

    void resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref) {
        if (!t.name || t.name->parts.empty()) {
            ref.kind = TypeRef::Kind::Error;
            return;
        }
        std::string_view first = t.name->parts.front();
        // Primitive check first.
        if (t.name->parts.size() == 1) {
            PrimitiveType prim = primitiveFromName(first);
            if (prim != PrimitiveType::None) {
                ref.kind = TypeRef::Kind::Primitive;
                ref.primitive = prim;
                // Primitives shouldn't have generic args (sema enforces);
                // we still resolve them so the AST is fully visited.
                for (const auto& ga : t.genericArgs) {
                    if (ga) resolveType(*ga, scope);
                }
                if (!t.genericArgs.empty()) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "primitive type cannot take generic arguments");
                }
                return;
            }
        }
        // Look up the first part.
        Symbol* sym = lookupName(first, scope);
        if (!sym) {
            // Unknown — but a null lookup may also mean "name is in scope as
            // a generic-param marker" (we used nullptr to indicate that).
            // Distinguish by checking if the key exists.
            bool isGeneric = false;
            for (Scope* s = scope; s; s = s->parent) {
                if (s->names.count(first)) { isGeneric = true; break; }
            }
            if (isGeneric) {
                // Generic parameter — record as a Named with no declaration.
                // Sema slice 3+ will treat generic-param TypeRefs specially.
                ref.kind = TypeRef::Kind::Named;
                ref.declaration = nullptr;
                return;
            }
            // Slice S24 (§2.3): always-available built-in type names
            // resolve without import. They have no user TypeSymbol, so
            // we record a Named ref with a null declaration and verify
            // generic arity against the spec's declared form.
            if (t.name->parts.size() == 1) {
                int arity = builtinTypeArity(first);
                if (arity != kNotBuiltin) {
                    ref.kind = TypeRef::Kind::Named;
                    ref.declaration = nullptr;
                    int n = static_cast<int>(t.genericArgs.size());
                    bool ok = (arity == kBuiltinTaskLike) ? (n == 0 || n == 1)
                                                          : (n == arity);
                    if (!ok) {
                        ctx_.diagnostics().report(
                            Severity::Error, t.span.begin,
                            std::string("generic-argument count does not "
                                        "match built-in type `") +
                            std::string(first) + "`");
                    }
                    for (const auto& ga : t.genericArgs) {
                        if (!ga) continue;
                        ref.genericArgs.push_back(resolveType(*ga, scope));
                    }
                    return;
                }
            }
            ctx_.diagnostics().report(
                Severity::Error, t.span.begin,
                std::string("unresolved type name `") +
                std::string(first) + "`");
            ref.kind = TypeRef::Kind::Error;
            return;
        }
        // For multi-part QualifiedNames (`Color.Red` style), we'd descend
        // into the looked-up symbol's members. Slice 2 supports only the
        // single-identifier case and a one-deep dotted name (e.g.
        // `System.Object` resolving to Object). For now we accept the
        // first part and ignore the rest if it's a TypeSymbol (the only
        // dotted-name case sema slice 2 needs is `Module.Name`-style).
        auto* tsym = dynamic_cast<TypeSymbol*>(sym);
        if (!tsym) {
            // Could be a TypeAliasSymbol or other. For slice 2, treat as
            // a Named pointing to no declaration (sema slice 3+ resolves
            // aliases).
            ref.kind = TypeRef::Kind::Named;
            ref.declaration = nullptr;
        } else {
            ref.kind = TypeRef::Kind::Named;
            ref.declaration = tsym;
            // Generic-arity check.
            if (!t.genericArgs.empty() &&
                t.genericArgs.size() != tsym->genericParams.size()) {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    "generic-argument count does not match type's "
                    "generic-parameter count");
            }
        }
        for (const auto& ga : t.genericArgs) {
            if (!ga) continue;
            TypeRef* arg = resolveType(*ga, scope);
            ref.genericArgs.push_back(arg);
        }
    }
};

void Resolver::resolveTypesInBody(
    const std::vector<std::unique_ptr<ast::Stmt>>& body, Scope* scope) {
    for (const auto& s : body) {
        if (s) resolveTypesInStmt(*s, scope);
    }
}

void Resolver::resolveTypesInStmt(const ast::Stmt& s, Scope* scope) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->type) resolveType(*ld->type, scope);
        if (ld->initializer) resolveTypesInExpr(*ld->initializer, scope);
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->type) resolveType(*lc->type, scope);
        if (lc->value) resolveTypesInExpr(*lc->value, scope);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) resolveTypesInExpr(*rs->value, scope);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) resolveTypesInExpr(*es->expr, scope);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target) resolveTypesInExpr(*as->target, scope);
        if (as->value) resolveTypesInExpr(*as->value, scope);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) resolveTypesInExpr(*ds->value, scope);
        return;
    }
    if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (tr->value) resolveTypesInExpr(*tr->value, scope);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            resolveTypesInExpr(*ifs->ifBranch.condition, scope);
        resolveTypesInBody(ifs->ifBranch.body, scope);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) resolveTypesInExpr(*el.condition, scope);
            resolveTypesInBody(el.body, scope);
        }
        resolveTypesInBody(ifs->elseBody, scope);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) resolveTypesInExpr(*ws->condition, scope);
        resolveTypesInBody(ws->body, scope);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->varType) resolveType(*fs->varType, scope);
        if (fs->iterable) resolveTypesInExpr(*fs->iterable, scope);
        if (fs->withExpr) resolveTypesInExpr(*fs->withExpr, scope);
        resolveTypesInBody(fs->body, scope);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        resolveTypesInBody(dob->body, scope);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        resolveTypesInBody(ts->body, scope);
        for (const auto& arm : ts->catches) {
            // Resolve the catch pattern's type(s) and guard, not just the
            // body — else `catch T => …` leaves T with no resolved
            // TypeRef and codegen cannot match the arm.
            for (const auto& ty : arm.types) {
                if (ty) resolveType(*ty, scope);
            }
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.body) resolveTypesInStmt(*arm.body, scope);
        }
        resolveTypesInBody(ts->finallyBody, scope);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) resolveTypesInExpr(*is->target, scope);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) resolveTypesInExpr(*ys->value, scope);
        return;
    }
}

void Resolver::resolveTypesInExpr(const ast::Expr& e, Scope* scope) {
    // First handle expressions that own a Type node directly.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (ne->type) resolveType(*ne->type, scope);
        for (const auto& a : ne->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->testType) resolveType(*te->testType, scope);
        if (te->operand) resolveTypesInExpr(*te->operand, scope);
        return;
    }
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        if (to->queriedType) resolveType(*to->queriedType, scope);
        return;
    }
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        if (de->type) resolveType(*de->type, scope);
        return;
    }
    // Recurse into children for everything else.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) resolveTypesInExpr(*be->left, scope);
        if (be->right) resolveTypesInExpr(*be->right, scope);
        return;
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->operand) resolveTypesInExpr(*ue->operand, scope);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) resolveTypesInExpr(*ma->target, scope);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nc->target) resolveTypesInExpr(*nc->target, scope);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) resolveTypesInExpr(*ce->callee, scope);
        for (const auto& a : ce->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) resolveTypesInExpr(*nc->callee, scope);
        for (const auto& a : nc->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) resolveTypesInExpr(*ix->target, scope);
        for (const auto& a : ix->indices) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) resolveTypesInExpr(*bg->operand, scope);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (ce->condition) resolveTypesInExpr(*ce->condition, scope);
        if (ce->thenValue) resolveTypesInExpr(*ce->thenValue, scope);
        if (ce->elseValue) resolveTypesInExpr(*ce->elseValue, scope);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) resolveTypesInExpr(*th->operand, scope);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // Lambda parameter types are themselves Types when present.
        for (const auto& p : lam->params) {
            if (p.type) resolveType(*p.type, scope);
        }
        if (lam->exprBody) resolveTypesInExpr(*lam->exprBody, scope);
        for (const auto& s : lam->stmts) {
            if (s) resolveTypesInStmt(*s, scope);
        }
        return;
    }
    if (auto* cae = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cae->scrutinee) resolveTypesInExpr(*cae->scrutinee, scope);
        for (const auto& arm : cae->arms) {
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.exprBody) resolveTypesInExpr(*arm.exprBody, scope);
            for (const auto& s : arm.stmts) {
                if (s) resolveTypesInStmt(*s, scope);
            }
        }
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) resolveTypesInExpr(*we->source, scope);
        for (const auto& ia : we->assigns) {
            if (ia.value) resolveTypesInExpr(*ia.value, scope);
        }
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys) {
            if (k) resolveTypesInExpr(*k, scope);
        }
        for (const auto& v : cl->elements) {
            if (v) resolveTypesInExpr(*v, scope);
        }
        return;
    }
    // §18.5: InterpolatedStringExpr — each expression part may contain type
    // nodes (e.g. `{x is IAnimal}` has a testType; `{new T(...)}`has a new-type).
    // Resolve them so that lowerTypeTest / lowerNew find the resolved TypeRef.
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : is->parts) {
            if (part.expr) resolveTypesInExpr(*part.expr, scope);
        }
        return;
    }
    // Literals + Identifier + ThisBaseTypeOf — no nested Types/Exprs.
}

} // namespace

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
