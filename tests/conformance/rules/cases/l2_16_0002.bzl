//! case: l2_16_0002
//! doc: stdlib
//! section: 2.16
//! title: Get() returns a borrow of the underlying value
//! rule: Shared<T>.Get() returns a borrow of the underlying value, sourced
//!   from the handle, valid while that handle lives.
//! cites: 2.16
//! expect: run
//! stdout: 7
module Conf;

public class Widget
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var s := new Shared<Widget>(new Widget(7));
    Console.WriteLine(s.Get().Id);
  end Main;
end Program;
