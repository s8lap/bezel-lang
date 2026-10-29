//! case: s28_2_0016
//! doc: spec
//! section: 28.2
//! title: a class nested in a class that declares Dispose with an optional parameter is rejected
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error** at that declaration, whatever its parameter list; at an
//!   optional-parameter declaration `Dispose(mode: int := 0)` it is additionally
//!   ambiguous at every zero-argument call.
//! cites: 8.7, 22.5, 28.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7493
module Conf;

public class Outer
  public class C
    public method Dispose(mode: int := 0)
    end Dispose;
  end C;
end Outer;
