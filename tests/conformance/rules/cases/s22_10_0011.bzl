//! case: s22_10_0011
//! doc: spec
//! section: 22.10
//! title: a class-typed scoped primary-constructor parameter is admitted and captured as a non-owned field
//! rule: A class- or interface-typed `scoped` primary-constructor parameter
//!   follows the same capture rule: referenced in a body position, it is
//!   captured as a compiler-generated non-owned private field — exactly the
//!   direct non-owned field of the bounding object that §28.14's callee side
//!   admits, the constructed object being the bounding object. At the call
//!   site the argument carries the marker — `new Box(scoped src)` — and is
//!   admitted under §28.14's bounding-object forms exactly as at a declared
//!   constructor: the holder is a `using var` local or `this`, the source is
//!   declared before the holder.
//! cites: 22.10, 28.14, 22.3
//! expect: accept
//! issue: 4601
module Conf;

public class Source
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Source;

public class Box(scoped src: Source)
  public method Touch()
    src.Ping();
  end Touch;
end Box;

public class Program
  public method Demo()
    using var s := new Source();
    using var b := new Box(scoped s);
    b.Touch();
  end Demo;
end Program;
