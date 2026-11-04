//! case: s28_2_0007
//! doc: spec
//! section: 28.2
//! title: a class member named Dispose is rejected at a non-empty parameter list
//! rule: Declaring a member named `Dispose` on a class, a record, an enum or a
//!   choice is a compile error at that declaration, whatever its parameter list
//!   and whatever its kind … On a class the name is the compiler's … so a user
//!   member of that name is never run, and at an optional-parameter
//!   declaration (`Dispose(mode: int := 0)`, §22.5) it is additionally
//!   ambiguous at every zero-argument call.
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
