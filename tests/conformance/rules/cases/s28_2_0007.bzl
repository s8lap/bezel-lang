//! case: s28_2_0007
//! doc: spec
//! section: 28.2
//! title: a class member named Dispose is rejected at a non-empty parameter list
//! rule: Declaring a member named `Dispose` on a class or a record is a
//!   compile error at that declaration, whatever its parameter list; the rule
//!   is keyed on the name, and at an optional-parameter declaration
//!   `Dispose(mode: int := 0)` it is additionally ambiguous against the
//!   compiler-generated `Dispose()` at every zero-argument call.
//! cites: 28.2, 22.5, 9.3
//! expect: reject
//! diagnostic: dispose
//! issue: 4404
module Conf;

public class Session
  public constructor()
  end constructor;

  public method Dispose(mode: int := 0)
    Console.WriteLine(mode.ToString());
  end Dispose;
end Session;
