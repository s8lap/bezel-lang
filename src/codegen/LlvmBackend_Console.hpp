#pragma once

// fix #9213 (stdlib §2.1 / §4.6 / §28.3 / §28.9, v0.359): the ONE classifier
// for the built-in `Console.ReadLine(): string?` call.
//
// WHY THIS HEADER EXISTS. One question about the built-in console's
// read-a-line member is asked from TWO translation units, and the two answers
// must agree:
//
//   1. the LOWERING (LlvmBackend_Calls2.cpp): "is this call the built-in
//      `Console.ReadLine()`?" — the built-in `Console` carries NO member
//      surface in sema (its members are matched structurally by the
//      lowerings, exactly as `tryLowerConsoleCall` matches
//      `Console.Out.WriteLine(x)`), so the ordinary member dispatch cannot
//      find the call and an arm must recognise it STRUCTURALLY;
//
//   2. the CONSUMER-SIDE CLASSIFICATION (LlvmBackend_Setup.cpp's
//      `Emitter::isPlusOneStringProducer`): "does this call mint a FRESH
//      `string` the CALLER owns?" — `emitConsoleReadLine` mints the result the
//      way every fresh string production is minted: a `malloc`'d `bz.string`
//      header whose rc is set to 1 (`initStringRc`), so the caller holds
//      exactly ONE reference and must discharge it (fix #9213: without the arm,
//      the bound local never released it and the read line leaked once per
//      read).
//
// This header is the ONE place both read, so the two cannot drift — the same
// discipline `LlvmBackend_TextReader.hpp` states for the §7.4 reader's own
// member, and `LlvmBackend_IndexerParams.hpp` for the indexer's index-
// parameter shape.
//
// THE RECEIVER. stdlib §2.1 (v0.359) declares, on the built-in `Console`,
//
//   public static method ReadLine(): string?;     // null at EOF
//
// The receiver is the built-in CLASS `Console` itself, read SYNTACTICALLY —
// the same "syntactic receiver" treatment `Console.In` / `Console.Out` /
// `Console.Error` get (§2.1's rule, of which the console's own properties are
// instances), and exactly as `tryLowerConsoleCall` recognises
// `Console.Out.WriteLine(x)`.
//
// ⇒ A VALUE named `Console` is member access, not the built-in class (fix
// #8126, §2.7 rule 1, v0.355): the receiver test asks sema whether the
// identifier may reach the built-in static arm.
//
// ⇒ A USER class named `Console` has its OWN member surface and its own
// dispatch: `receiverMayReachBuiltinStaticArm` is false for it, so the
// classifier declines it.
//
// THE NULL AT EOF. The result is `string?` and is NULL at end of input. The
// classifier says only WHETHER the call mints a fresh production — never
// whether this particular run produced a value. A NULL result carries no
// reference, and the runtime's string release is null-safe, so the
// consumer-side release needs no extra guard.

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

namespace bezel::codegen {

// Is `call` the built-in `Console.ReadLine(): string?` member call? (§2.1)
inline bool isBuiltinConsoleReadLine(const ast::CallExpr& call,
                                     const sema::SemaContext& ctx) {
    if (!call.callee) return false;
    // §2.1: `ReadLine()` takes no argument.
    if (!call.args.empty()) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return false;
    if (ma->member != "ReadLine") return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    if (!id || id->name != "Console") return false;
    // fix #8126 (§2.7 rule 1, v0.355): a VALUE named `Console` is member
    // access, not the built-in class.
    return sema::receiverMayReachBuiltinStaticArm(ctx.exprSymbol(*id));
}

}  // namespace bezel::codegen
