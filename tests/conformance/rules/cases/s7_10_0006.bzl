//! case: s7_10_0006
//! doc: spec
//! section: 7.10
//! title: a non-nullable reference type satisfies where T: class
//! rule: `where T: class` restricts T to non-nullable reference types —
//!   classes, interfaces, `string`, function types.
//! cites: 14.2
//! expect: run
//! stdout: ok
module Conf;

public class Crate<T> where T: class
  public constructor()
  end constructor;
end Crate;

public class Program
  public static method Main()
    var c := new Crate<string>();
    Console.WriteLine("ok");
  end Main;
end Program;
