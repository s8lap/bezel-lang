//! case: s18_1_0001
//! doc: spec
//! section: 18.1
//! title: digit separators are ignored and integer suffixes are case-insensitive
//! rule: "`1_234_567` // digit separators allowed except at start/end" and
//!   "Suffixes are case-insensitive: `L`/`l`, `U`/`u`, `F`/`f`, `M`/`m`."
//! cites: 18.1
//! expect: run
//! stdout: 1234567
module Conf;

public class Program
  public static method Main()
    var total := 1_234_567l;
    Console.WriteLine(total);
  end Main;
end Program;
