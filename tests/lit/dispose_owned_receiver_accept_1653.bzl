// Clean-compile ACCEPT control for issue #1653.
//
// Spec §28.6 (anchor sec-who-may-dispose, v0.184): an OWNED local is the
// canonical admitted `.Dispose()` receiver. `var x := new Resource(); x.Dispose();`
// — a fresh owned producer bound to a plain owned local, then disposed — must
// compile clean: no §28.6 receiver-ownership diagnostic, no error of any kind.
// This is the accept side of the reject in dispose_borrow_receiver_1653.bzl: it
// proves the new §28.6 receiver check does NOT over-reject a genuine owned
// receiver.
//
// Two RUN lines assert the accept two ways:
//  1. `bezelc -c` must EXIT ZERO — lit fails the test on a non-zero return, so a
//     successful compile IS the pass. Output goes to `%t.o` (build-tree scratch).
//  2. The diagnostic stream must be empty of errors — FileCheck with
//     `--allow-empty` confirms neither "error" nor a § section-citation reject.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class Resource public method Ping(): () end Ping; end Resource;
class Program
  public static method Main(): int
    var x := new Resource();
    x.Dispose();
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
