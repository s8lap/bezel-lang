// Issue #6518 — the spec's own affordance for an argument-position identifier
// that names a real type. §26.4 (spec v0.338) binds every bare
// single-segment non-nullable identifier in the extended type's argument
// positions as a fresh block-level type parameter, ALWAYS, shadowing any
// same-named type in scope, "The alternative — resolving the identifier
// against scope and only binding when unresolved — is declined: it would let
// an unrelated type declaration elsewhere silently re-mean an extension block
// (action at a distance)."
//
// So `extension Array<Foo>` above a DECLARED `class Foo` is NOT an error — the
// block parameter wins — but the spec says the compiler "warns on such
// shadowing", and that warning is the whole of the language's answer to an
// author who meant the type. This is where the diagnostic lives: the lit track,
// because runAot fills `JitResult::diagnostics` only on a REJECT (#3138), so a
// Warning on a program that compiles is vacuous through the gtest oracle.
//
// Companion gtest rows: tests/codegen/extension_head_arg_is_type_param_6518_test.cpp

// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

class Foo public v: int; public constructor(v: int) this.v := v; end constructor; end Foo;

extension Array<Foo>
  public method M2(): int => 0;
end;

class C public static method Main(): int return 0; end Main; end C;

// CHECK: warning: type parameter `Foo` in this extension block shadows the type `Foo` that is in scope (§26.4); the extension type parameter always takes precedence