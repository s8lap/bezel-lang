//! case: s28_3_0184
//! doc: spec
//! section: 28.3
//! title: a type parameter directly in a function-typed parameter's slots, depth one, is admitted
//! rule: In a generic declaration — its signature and declared types ... and
//!   every type written in its bodies (a local's or a lambda's annotation, an
//!   `as`, `is` or pattern target, a type argument) — a type parameter, bare,
//!   nullable or within a type argument, does not occur inside a function
//!   type that is itself inside a function type: `reg: ((T) -> ()) -> ()`,
//!   `reg: (() -> T) -> ()`, ... and a body's `o as ((T) -> ()) -> ()` are
//!   compile errors naming this rule
//! cites: 28.3, 27.1
//! expect: accept
module Conf;

public class Reg<T>
  public constructor()
  end constructor;

  public method Fill(visit: (T) -> (), make: () -> T)
    visit(make());
  end Fill;
end Reg;
