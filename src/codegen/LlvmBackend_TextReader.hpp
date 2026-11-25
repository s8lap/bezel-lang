#pragma once

// fix #9199 (stdlib §7.4 / §2.1 / §4.6 / §28.3 / §28.9, v0.359): the ONE
// classifier for the built-in `TextReader.ReadToEnd(): string` call.
//
// WHY THIS HEADER EXISTS. One question about the built-in reader's
// read-to-EOF member is asked from TWO translation units, and the two answers
// must agree:
//
//   1. the LOWERING (LlvmBackend_Calls2.cpp): "is this call the built-in
//      `TextReader.ReadToEnd()`?" — the built-in `TextReader` carries NO
//      member surface in sema (it has no `TypeDecl`), so the ordinary member
//      dispatch cannot find the call and an arm must recognise it
//      STRUCTURALLY (fix #8383);
//
//   2. the CONSUMER-SIDE CLASSIFICATION (LlvmBackend_Setup.cpp's
//      `Emitter::isPlusOneStringProducer`): "does this call mint a FRESH
//      `string` the CALLER owns?" — the built-in reader's result is a fresh
//      production with `rc := 1` (`initStringRc`), so the caller holds
//      exactly ONE reference and must discharge it (fix #9199: without the
//      arm, the bound local never released it and the read text leaked once
//      per run).
//
// This header is the ONE place both read, so the two cannot drift — the same
// discipline `LlvmBackend_IndexerParams.hpp` states for the indexer's
// index-parameter shape.
//
// THE RECEIVER. stdlib §7.4 declares `ReadToEnd` on the abstract
// `TextReader`, and §2.1 declares `Console.In` as its one instance. Two
// spellings reach the member:
//
//   * `Console.In.ReadToEnd()` — the receiver is the built-in `Console`
//     class's `In` PROPERTY read. The read itself is left UNTYPED by sema
//     (§2.1's rule, of which `Console.In` is an instance — the same
//     "syntactic receiver" treatment `Console.Out` / `Console.Error` get), so
//     the receiver is recognised SYNTACTICALLY, exactly as `tryLowerConsoleCall`
//     recognises `Console.Out.WriteLine(x)`.
//
//   * `r.ReadToEnd()` on a value of the built-in `TextReader` type — the
//     receiver is recognised by its TYPE (a declaration-less Named ref whose
//     head is `TextReader`), the same discriminator `isBuiltinTextWriterTypeRef`
//     uses for the `TextWriter` family.
//
// ⇒ A USER class of the same name is excluded: `TextReader` is a built-in
// class head, so a user declaration of that name has its OWN member surface
// and its own dispatch.
//
// ⇒ `Console.In` is the only `TextReader` instance stdlib §7.4 admits, so
// BOTH receivers denote the same reader — standard input.

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"

namespace bezel::codegen {

// Which receiver the built-in `TextReader.ReadToEnd()` call has.
enum class TextReaderReceiverKind {
    None,       // not the built-in member — the ordinary dispatch owns it
    ConsoleIn,  // `Console.In.ReadToEnd()` — the syntactic receiver
    TypedValue, // `r.ReadToEnd()` on a value of the built-in `TextReader` type
};

// Is `call` the built-in `TextReader.ReadToEnd()` member call? (§7.4)
inline TextReaderReceiverKind classifyTextReaderReadToEnd(
    const ast::CallExpr& call, const sema::SemaContext& ctx) {
    if (!call.callee) return TextReaderReceiverKind::None;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return TextReaderReceiverKind::None;
    // §7.4: `ReadToEnd()` takes no argument.
    if (ma->member != "ReadToEnd" || !call.args.empty())
        return TextReaderReceiverKind::None;
    // A USER-declared `ReadToEnd` on a user receiver is the user's own
    // member: the ordinary dispatch owns it, and this arm must not claim it.
    if (dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*call.callee)))
        return TextReaderReceiverKind::None;
    // (1) `Console.In.ReadToEnd()` — the syntactic receiver.
    if (auto* inner =
            dynamic_cast<const ast::MemberAccessExpr*>(ma->target.get())) {
        if (inner->member == "In" && inner->target) {
            if (auto* cid = dynamic_cast<const ast::IdentifierExpr*>(
                    inner->target.get())) {
                // fix #8126 (§2.7 rule 1, v0.355): a VALUE named `Console` is
                // member access, not the built-in class.
                if (cid->name == "Console" &&
                    sema::receiverMayReachBuiltinStaticArm(
                        ctx.exprSymbol(*cid)))
                    return TextReaderReceiverKind::ConsoleIn;
            }
        }
    }
    // (2) a value of the built-in `TextReader` type.
    const sema::TypeRef* rt = ctx.exprType(*ma->target);
    if (rt && rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
        rt->headName() == "TextReader")
        return TextReaderReceiverKind::TypedValue;
    return TextReaderReceiverKind::None;
}

}  // namespace bezel::codegen
