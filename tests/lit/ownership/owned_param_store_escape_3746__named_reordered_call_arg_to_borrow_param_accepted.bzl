// §22.5 "Named arguments" / §22.3 "Parameter modifiers" / §28.3 (v0.286); the
// CALL-ARGUMENT accept control for issue #3746, added after gate cycle 6.
//
// §22.5: "positional arguments fill parameters in order; a named argument
// `name := expr` fills the parameter named `name`; named arguments may appear
// in any order after positional arguments."
//
// #3746 made a borrow of an `owned` parameter a rejected store at four
// destinations that OUTLIVE the method activation (a field, an out/ref
// parameter, and the two §28.11 collection stores on an escaping receiver). An
// ARGUMENT position is none of those: the callee's `b` is a plain borrow
// parameter, so the call borrows `p` for its duration and stores nothing that
// can outlive it. `p` sits at source index 0 here and binds `b` by name, so the
// row also pins that the §22.5 reorder does not relabel the argument into a
// destination the escape check inspects.
//
// This must compile and print 12.
//
// The NODIAG pattern below is deliberately NARROWER than the two sibling accept
// rows' `NODIAG-NOT: §`: this program leaves `p` undischarged, so it carries the
// ordinary §28.3 *Auto-drop (affine)* must-use WARNING ("owned parameter `p` is
// implicitly disposed at the end of the method"), which is a lint over a sound
// program and is emitted on the parent too. Discharging `p` to silence it would
// destroy the row — an argument passed after a `move` is no longer a borrow of a
// live owned parameter, which is the thing being pinned. So the NOT pattern
// names the diagnostic that must NOT appear instead.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class C
  public static method Peek2(a: int, b: R): int
    return b.V() + a;
  end Peek2;
  public static method Take(p: owned R): int
    return Peek2(b := p, a := 7);
  end Take;
  public static method Main(): int
    Console.WriteLine(Take(new R()).ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §28.3's borrowed-field store rule
