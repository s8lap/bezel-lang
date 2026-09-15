//! case: s21_10_0006
//! doc: spec
//! section: 21.10
//! title: an interface may not be declared as a local type
//! rule: "`class` and `interface` may not [be declared locally] — … an
//!   interface is a contract for an outside a local has none of."
//! cites: 21.10, C.1
//! expect: reject
//! diagnostic: interface
module Conf;

public class Program
  public static method Main()
    interface IMarker
      method Ping(): int;
    end IMarker;
    Console.WriteLine("done");
  end Main;
end Program;
