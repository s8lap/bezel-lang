//! case: s28_2_0020
//! doc: spec
//! section: 28.2
//! title: a class nested in a generic class that declares Dispose is rejected
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error** at that declaration, whatever its parameter list; the
//!   class is rejected whatever the type parameters of its enclosing class.
//! cites: 28.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7493
module Conf;

public class Outer<T>
  public class C
    public method Dispose()
    end Dispose;
  end C;
end Outer;
