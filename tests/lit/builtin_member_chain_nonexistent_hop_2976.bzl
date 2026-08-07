// fix #2976 (codegen, stdlib §6.5 `EventSource<T>`, language spec §3/#605,
// v0.240): a NON-EXISTENT member reached as an intermediate HOP of a member
// chain on a built-in receiver.
//
//   using var sub := src.Event.Subscribe(Handle);   // `Event` names nothing
//
// stdlib §6.5 declares `EventSource<T>`'s whole surface (`Subscribe`, `Raise`,
// `Add`, `Remove`, a destructor) — there is no `Event` member — so `src.Event`
// names nothing and the REAL `Subscribe` was applied to that missing value.
// Before this fix the whole initializer degraded to a zero-initialized slot
// behind #2434's `internal:` WARNING, the build succeeded, and the program ran
// to exit 0 with the subscription never created and the handler never fired.
//
// This lit test is the home for the two things the AOT gtest harness cannot
// assert, because on a compile reject it packs the entire driver stderr into
// one diagnostic with an EMPTY location (tests/support/aot_support.hpp, "S1"):
//
//   1. the diagnostic has a REAL source location, not 0:0 — checked by the
//      `:{{[0-9]+}}:{{[0-9]+}}:` prefix, which cannot match a zero line;
//   2. the diagnostic names `Event`, the hop that actually failed, and says in
//      so many words that naming `Subscribe` would be wrong. `Subscribe` is
//      documented AND implemented (the gtest control in
//      tests/codegen/builtin_member_chain_2976_test.cpp runs it and the handler
//      fires), so a message blaming it would be a correct rejection carrying a
//      false claim about the program.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System.Threading;

class C
  public static method Main(): int
    using var src := new EventSource<int>();
    using var sub := src.Event.Subscribe(Handle);
    src.Raise(7);
    Console.WriteLine("reached end");
    return 0;
  end Main;

  public static method Handle(e: int)
    Console.WriteLine("handler fired");
  end Handle;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Event` on built-in type `EventSource` produced no value, so it cannot initialize this local.
// CHECK-SAME: the failing hop is `Event`
// CHECK-SAME: was then applied to its missing value, so naming `Subscribe` here would be wrong
