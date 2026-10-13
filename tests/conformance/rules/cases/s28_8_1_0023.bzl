//! case: s28_8_1_0023
//! doc: spec
//! section: 28.8.1
//! title: a with clause on the move of a local binding is rejected
//! rule: The "with" Expression clause is the REPLACING move of §28.8.1,
//!   admitted at (a) and (b) only ... At (c) a `with` clause is a compile
//!   error.
//! cites: 28.8.1, 28.3
//! expect: reject
//! diagnostic: with
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Program
  public method Demo()
    var a := new Res();
    using var t := move a with new Res();
  end Demo;
end Program;
