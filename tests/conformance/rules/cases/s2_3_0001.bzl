//! case: s2_3_0001
//! doc: spec
//! section: 2.3
//! title: several always-available names are usable with no import
//! rule: The following names are visible in every source file without
//!   import — including `System`'s `Console`, `System.Text`'s
//!   `StringBuilder`, and `System.Collections.Generic`'s `Array<T>`.
//! cites: 2.3
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    sb.Append("hi");
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(xs.Count);
  end Main;
end Program;
