//! case: s5_1_0139
//! doc: spec
//! section: 5.1
//! title: a byte index widens to an int parameter and keeps its value
//! rule: A value of one type implicitly converts to another when no
//!   programmer-visible information is lost, and the conversion table lists
//!   `int` among the implicit targets of `byte`; widening from unsigned to a
//!   strictly larger signed type is permitted because no value is lost. The
//!   stdlib declares the parameter as `int`: `Substring(startIndex: int)`, so
//!   the byte 128 reaches the call as the int 128 and the substring starts at
//!   the byte's own value.
//! cites: 5.1, stdlib 9.6
//! expect: run
//! stdout: 124
//! issue: 7102
module Conf;

public class Program
  public static method Main(): int
    var s: string := "c" + String.Repeat("x", 250) + "c";   // 252 characters
    var v: byte := 128;
    Console.WriteLine(s.Substring(v).Length.ToString());
    return 0;
  end Main;
end Program;
