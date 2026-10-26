//! case: l2_3_0018
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString with a runtime toBase renders the radix form
//! rule: stdlib §2.3: "`ToString(value, toBase)` returns the text
//!     `value.ToString(toBase)` returns (§9.1)". §9.1: "`ToString(radix)` emits
//!     the **lower-case** digits". The `toBase` is a run-time value here, and
//!     the rendered text is the radix form, not the decimal form.
//! cites: 2.3, 9.1
//! expect: run
//! stdout: ff
module Conf;

public class Program
  public static method Main()
    var r: int := 16;
    Console.WriteLine(Convert.ToString(255, r));
  end Main;
end Program;
