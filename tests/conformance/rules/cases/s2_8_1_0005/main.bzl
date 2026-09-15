//! case: s2_8_1_0005
//! doc: spec
//! section: 2.8.1
//! title: a root module name distinct from every dependency is accepted
//! rule: A build's artifact set is ill-formed only when two distinct
//!   artifacts declare the same module name; a distinct root module name
//!   alongside a dependency is unaffected.
//! cites: 2.8.1, C.13
//! expect: accept
module App;
import Lib;

public class Bar
  public constructor()
  end constructor;

  public method Make(): owned Foo => new Foo();
end Bar;
