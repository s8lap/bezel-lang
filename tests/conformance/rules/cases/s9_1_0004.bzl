//! case: s9_1_0004
//! doc: spec
//! section: 9.1
//! title: positional-record ToString renders TypeName(Field1 colon value1, ...)
//! rule: ToString is formatted as "TypeName(Field1: value1, Field2: value2)".
//! expect: run
//! stdout: Person(Name: Ada, Age: 30)
module Conf;

public record Person(Name: string, Age: int);

public class Program
  public static method Main()
    var p := new Person("Ada", 30);
    Console.WriteLine(p.ToString());
  end Main;
end Program;
