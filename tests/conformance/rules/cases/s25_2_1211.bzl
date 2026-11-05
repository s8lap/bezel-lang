//! case: s25_2_1211
//! doc: spec
//! section: 25.2
//! title:a user record named TimeSpan shadows the stdlib TimeSpan and its own double-left overload
//!   applies
//! rule:a binary overload may pair the declaring type with any other type in either position
//! cites: 2.3
//! expect: run
//! stdout: 10
module Conf;

public record TimeSpan(T: long)
  public static method operator *(d: double, t: TimeSpan): TimeSpan
    return new TimeSpan(long(d) * t.T);
  end operator *;
end TimeSpan;

public class Program
  public static method Main()
    var a := new TimeSpan(5L);
    var r := 2.0 * a;
    Console.WriteLine(r.T);
  end Main;
end Program;
