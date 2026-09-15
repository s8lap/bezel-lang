//! case: s5_1_0005
//! doc: spec
//! section: 5.1
//! title: ushort implicitly converts to int (unsigned widens to a strictly larger signed type)
//! rule: Widening from unsigned to a strictly larger signed type (e.g. byte ->
//!   short, ushort -> int) is permitted because no value is lost.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(u: ushort)
    var n: int := u;
  end Convert;
end Program;
