//! case: s28_2_0018
//! doc: spec
//! section: 28.2
//! title: a class nested in a class that declares a parameterless Dispose is rejected
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error** at that declaration, whatever its parameter list; the
//!   nesting of the class does not change it.
//! cites: 28.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7493
module Conf;

public class Outer
  public class C
    public method Dispose()
    end Dispose;
  end C;
end Outer;
