//! case: s27_8_0001
//! doc: spec
//! section: 27.8
//! title: a type alias is a pure synonym interchangeable with the aliased type
//! rule: A type alias is a pure synonym — it does not create a new type;
//!   `UserId` and `int` are interchangeable.
//! cites: 27.8
//! expect: run
//! stdout: 42
module Conf;

type UserId := int;

public class Program
  public static method Main()
    var id: UserId := 42;
    var raw: int := id;
    Console.WriteLine($"{raw}");
  end Main;
end Program;
