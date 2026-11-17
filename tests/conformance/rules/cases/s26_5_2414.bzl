//! case: s26_5_2414
//! doc: spec
//! section: 26.5
//! title: an extension method named Count on a class that declares no Count
//!   member is resolved
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
//! cites: 26.5
//! expect: run
//! stdout: 5
module Conf;

public class Bag
  public method Add2(): int => 1;
end Bag;

extension Bag
  public method Count(): int => 5;
end;

public class Program
  public static method Main()
    var b := new Bag();
    Console.WriteLine(b.Count());
  end Main;
end Program;
