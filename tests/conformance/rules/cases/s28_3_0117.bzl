//! case: s28_3_0117
//! doc: spec
//! section: 28.3
//! title: a static method reference stored into a bare-T field at a function-type instantiation is admitted
//! rule: A **set-erasing position** is ... a field (instance or static; not a
//!   `scoped` field ...), parameter, return slot, ... whose declared,
//!   pre-substitution type ... is a bare type parameter, at an instantiation
//!   substituting a function type or `Object`, nullable or not, for it — ...
//!   `b.V := f` at `b: Box<Object>` or `b: Box<() -> int>` over `class Box<T>
//!   public V: T; end Box;` (the same store in a class deriving from either
//!   included) ... At every set-erasing position **a function value with a
//!   non-empty source set is rejected, and a capture-free one is admitted**
//! cites: 28.3, 27.6
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Box<T>
  public V: T;

  public constructor(v: T)
    V := v;
  end constructor;
end Box;

public class Client
  public static method One(): int => 1;

  public static method Two(): int => 2;

  public static method Run(): int
    using var b := new Box<() -> int>(Client.One);
    b.V := Client.Two;
    var g := b.V;
    return g();
  end Run;
end Client;
