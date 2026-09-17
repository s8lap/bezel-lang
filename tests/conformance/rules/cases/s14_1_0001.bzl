//! case: s14_1_0001
//! doc: spec
//! section: 14.1
//! title: a generic class parameterizes its members over a type parameter
//! rule: Classes, interfaces, records, and methods may be generic; type
//!   parameters use angle brackets (§14.1).
//! cites: 14.1
//! expect: run
//! stdout: 5
module Conf;

public class Box<T>
  private _item: T;

  public constructor(item: T)
    _item := item;
  end constructor;

  public method Get(): T => _item;
end Box;

public class Program
  public static method Main()
    var b := new Box<int>(5);
    Console.WriteLine(b.Get());
  end Main;
end Program;
