//! case: s17_4_0004
//! doc: spec
//! section: 17.4
//! title: as on a value-type target produces a nullable result
//! rule: "When the target type is a value type, `x as T` produces `T?` ... So
//!   `someObj as int` has type `int?`."
//! cites: 17.4
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    var obj: Object := 42;
    var r := obj as int;
    Console.WriteLine(r!);
  end Main;
end Program;
