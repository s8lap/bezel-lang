//! case: s29_2_0002
//! doc: spec
//! section: 29.2
//! title: the for variable over string-typed elements is a shared reference
//! rule: What the `for` variable is, decided by the static type of the
//!   source: `string`- or function-typed elements are a shared reference.
//! cites: 29.2
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var names: Array<string> := ["ab", "xyz"];
    var total := 0;
    for var s in names do
      total := total + s.Length;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
