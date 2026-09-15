//! case: s17_8_0002
//! doc: spec
//! section: 17.8
//! title: there is no prefix ! operator — boolean negation is not
//! rule: "`!` is the only postfix `!` operator. There is no prefix `!`;
//!   boolean negation is `not`."
//! cites: 17.8
//! expect: reject
//! diagnostic: not
module Conf;

public class Program
  public static method Main()
    var b := true;
    var r := !b;
  end Main;
end Program;
