//! case: s7_9_0006
//! doc: spec
//! section: 7.9
//! title: a nullable local declared without an initializer holds null
//! rule: A local of defaultable type can be declared without an initializer —
//!   it gets the type's default, `null` for a nullable local.
//! cites: 17.11
//! expect: run
//! stdout: defaulted
module Conf;

public class Program
  public static method Main()
    var label: string?;
    if label = null then
      Console.WriteLine("defaulted");
    else
      Console.WriteLine("not defaulted");
    end;
  end Main;
end Program;
