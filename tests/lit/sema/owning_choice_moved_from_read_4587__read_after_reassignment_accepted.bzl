// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #4587 (§15.3 / §28.7, spec v0.306) accept control: a read AFTER
// reassignment must still be admitted. §15.3's first Rules bullet states the
// read is rejected only "until reassigned" — once `b` is given a fresh value
// via a second local-to-local move (`c := b;`) followed by a plain
// reassignment (`b := Bundle.Ready(...)`), the moved-from state is cleared
// and the subsequent `case b of ...` reads a live value again. This guards
// against the fix over-rejecting: a broken implementation that left `b`
// permanently poisoned once moved-from (never re-armed by a reassignment)
// would fail this file with the SAME diagnostic the sibling reject tests
// pin, on the `case b of` line below.

module M;
class Resource
  public constructor() end constructor;
  destructor end destructor;
end Resource;
choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;
class C
  public static method Main(): int
    var b := Bundle.Ready(new Resource());
    var c := Bundle.Ready(new Resource());
    c := b;
    b := Bundle.Ready(new Resource());
    case b of
      Ready(_) => do end,
      Empty => do end
    end;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
