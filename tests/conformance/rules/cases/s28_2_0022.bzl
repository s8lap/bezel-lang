//! case: s28_2_0022
//! doc: spec
//! section: 28.2
//! title: an interface nested in a class may declare Dispose
//! rule: Declaring a member named `Dispose` on a **class** or a **record** is a
//!   **compile error**; "Interfaces are unaffected: the rule reaches classes and
//!   records only, and an interface may declare `Dispose()` itself." This is the
//!   accepting twin of s28_2_0018 for an interface nested in a class.
//! cites: 28.2
//! expect: accept
module Conf;

public class Outer
  public interface I
    method Dispose();
  end I;
end Outer;
