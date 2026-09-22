//! case: s22_10_0016
//! doc: spec
//! section: 22.10
//! title: a fresh producer at a scoped primary-constructor argument is rejected
//! rule: At the call site the argument carries the marker and is admitted
//!   under §28.14's bounding-object forms exactly as at a declared
//!   constructor: a fresh producer is rejected — the unbound owned temporary
//!   dies at the end of the full expression (§9.7's no-temporary-source
//!   rule); bind the source first.
//! cites: 22.10, 28.14, 9.7
//! expect: reject
//! diagnostic: temporary
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
    using var b := new Box(scoped new Source());
    b.Touch();
  end Demo;
end Program;
