//! case: s28_3_0200
//! doc: spec
//! section: 28.3
//! title: a body-formed lambda with T in its return slot converted to Object is rejected
//! rule: A function value the body forms or retypes at a function type that
//!   places one of its type parameters, bare or nullable, in a **return slot
//!   reached through return slots only** (`(Box<T>) -> T`, `() -> T`, `() ->
//!   () -> T`: `lambda(b: Box<T>) => b.V`, ...), which lends at that slot
//!   (*Calls mint* clause 7), likewise reaches **no set-erasing position** —
//!   not a conversion to `Object`, implicit or by an `as`
//! cites: 28.3, 12.7
//! expect: reject
//! diagnostic: set-erasing
//! issue: 5904
module Conf;

public class Box<T>
  public V: T;

  public constructor(v: T)
    V := v;
  end constructor;
end Box;

public class Maker<T>
  public constructor()
  end constructor;

  public method Hand(): int
    var o: Object := lambda(b: Box<T>) => b.V;
    return 1;
  end Hand;
end Maker;
