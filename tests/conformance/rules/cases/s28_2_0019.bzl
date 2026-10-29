//! case: s28_2_0019
//! doc: spec
//! section: 28.2
//! title: a generic class nested in a class that declares Dispose is rejected
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error** at that declaration, whatever its parameter list; a
//!   generic class is a class, so its `Dispose` is rejected at the declaration.
//! cites: 28.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7493
module Conf;

public class Outer
  public class G<T>
    public method Dispose()
    end Dispose;
  end G;
end Outer;
