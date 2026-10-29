//! case: s28_2_0014
//! doc: spec
//! section: 28.2
//! title: a record nested in a class that declares a member named Dispose is rejected
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error** at that declaration, whatever its parameter list; the
//!   rule is keyed on the name, and the nesting of the record does not change it.
//! cites: 8.7, 9.7, 28.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7493
module Conf;

public class Outer
  public record R
    public method Dispose()
    end Dispose;
  end R;
end Outer;
