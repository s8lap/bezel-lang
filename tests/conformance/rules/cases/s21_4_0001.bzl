//! case: s21_4_0001
//! doc: spec
//! section: 21.4
//! title: for over an inline array is a direct indexed loop over value copies
//! rule: "`for` iterates any iterable source, including inline arrays
//!   `[N]T`, which support direct indexed iteration through a compiler
//!   intrinsic."
//! cites: 21.4, 15.1.2
//! expect: run
//! stdout: 255
module Conf;

public class Program
  public static method Main()
    var rgb: [3]byte := [255, 128, 64];
    for var b in rgb do
      Console.WriteLine(b.ToString());
    end;
  end Main;
end Program;
