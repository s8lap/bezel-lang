//! case: s28_10_0013
//! doc: spec
//! section: 28.10
//! title: using var over an if arm-meet of two unmarked owned locals is rejected
//! rule: When **every** arm of the meet — `null`-literal and `throw` arms set
//!   aside — is an unmarked read of a class- or interface-typed named owned
//!   binding, and at least one arm is, the `using var` is **rejected** on that
//!   paragraph's ground: `using var y := (if c then a else b);` and
//!   `using var y := a ?? b;` over two owned locals say neither that `y`
//!   adopts the selected binding nor that it views it, and the compiler does
//!   not choose.
//! cites: 28.10, 28.3, 20
//! expect: reject
//! diagnostic: using var
//! issue: 5644
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Program
  public method Run(c: bool)
    var a := new Widget();
    var b := new Widget();
    using var held := (if c then a else b);
    held.Ping();
  end Run;
end Program;
