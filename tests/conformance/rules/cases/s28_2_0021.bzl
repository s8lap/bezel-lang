//! case: s28_2_0021
//! doc: spec
//! section: 28.2
//! title: a Dispose declared at three nesting levels draws the rule at each declaration
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error** at that declaration, whatever its parameter list; each
//!   class and record declaring it is its own compile error, at its own
//!   declaration, so no level is exempt for being nested inside another.
//! cites: 28.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7493
module Conf;

public class A
  public class B
    public record C
      public method Dispose()
      end Dispose;
    end C;
    public method Dispose()
    end Dispose;
  end B;
  public method Dispose()
  end Dispose;
end A;
