//! case: s14_5_4817
//! doc: spec
//! section: 14.5
//! title: a byte range and an int range at two IEnumerable<T> formals of one T disagree
//! rule: All bindings a type parameter receives
//! cites: 14.5, stdlib 9.8
//! expect: reject
//! diagnostic: cannot be bound to both
module Conf;

public class Program
  private static method Tot<T>(a: IEnumerable<T>, b: IEnumerable<T>): int
    return 0;
  end Tot;

  public static method Main()
    var a: byte := 1;
    var b: byte := 3;
    Console.WriteLine(Tot(a..b, 1..3));
  end Main;
end Program;
