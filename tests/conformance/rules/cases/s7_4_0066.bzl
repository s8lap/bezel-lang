//! case: s7_4_0066
//! doc: spec
//! section: 7.4
//! title: a finally narrows a nullable string inside its own null test and prints both lines
//! rule: "A fact needed inside an arm or a `finally` is established inside it (`if s <> null then …`), and one
//!   needed after a `try` with arms is carried by every arm or re-established after the statement" ([§7.4]).
//!   The test `s <> null` inside the `finally` narrows `s` for its body, so the read is legal and the
//!   `finally` prints its length, after the block has printed "a" through `Log`.
//! cites: 7.4, 31.2
//! expect: run
//! output: a
//!   1
//! issue: 6673
module Conf;

public class Program
  public static method Log(m: string)
    Console.WriteLine(m);
  end Log;

  public static method Main()
    var s: string? := null;
    try
      s := "x";
      Log("a");
    finally
      if s <> null then Console.WriteLine(s.Length); end;
    end try;
  end Main;
end Program;
