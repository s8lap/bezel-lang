// Clean-compile ACCEPT control for issue #1749.
//
// Spec §12.2 (v0.184): a derived class may reuse an inherited member's name
// when the declaration is an `override` of a `virtual` base member with a
// matching signature. `Inner` below is a class NESTED inside `Base` (§11.6)
// that derives from `Base` and overrides `Base`'s virtual `Greet` method —
// the correct `override` marker is present, so this must compile clean: no
// §12.2 hiding diagnostic, no error of any kind. This is the accept side of
// the reject in nested_class_hiding_1749.bzl: it proves the #1749 fix (running
// the §12.2 hiding/override checks for nested-class members too) does not
// over-reject a genuine, correctly-marked override just because the derived
// class is nested.
//
// Two RUN lines assert the accept two ways:
//  1. `bezelc -c` must EXIT ZERO — lit fails the test on a non-zero return,
//     so a successful compile IS the pass. Output goes to `%t.o` (build-tree
//     scratch), never the source tree.
//  2. The diagnostic stream must be empty of errors — FileCheck with
//     `--allow-empty` confirms neither "error" nor a § section-citation
//     reject appears.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class Base
  public virtual method Greet(): string => "hi";

  public class Inner : Base
    public override method Greet(): string => "hello";
  end Inner;
end Base;

// NODIAG-NOT: error
// NODIAG-NOT: §
