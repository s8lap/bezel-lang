//! case: s28_3_0183
//! doc: spec
//! section: 28.3
//! title: a body-written as target nesting a type parameter two function types deep is rejected
//! rule: In a generic declaration — its signature and declared types ... and
//!   every type written in its bodies (a local's or a lambda's annotation, an
//!   `as`, `is` or pattern target, a type argument) — a type parameter, bare,
//!   nullable or within a type argument, does not occur inside a function
//!   type that is itself inside a function type: `reg: ((T) -> ()) -> ()`,
//!   `reg: (() -> T) -> ()`, ... and a body's `o as ((T) -> ()) -> ()` are
//!   compile errors naming this rule
//! cites: 28.3, 27.1
//! expect: reject
//! diagnostic: depth one
//! issue: 5829
module Conf;

public class Reg<T>
  public constructor()
  end constructor;

  public method Probe(o: Object): bool
    var f := o as ((T) -> ()) -> ();
    return f <> null;
  end Probe;
end Reg;
