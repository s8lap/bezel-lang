//! case: s28_9_0904
//! doc: spec
//! section: 28.9
//! title: a chain call on a static method result that is a closure built by an inner block lambda
//! rule: **Function-typed values** — values of any function type `(T1, ..., Tn) -> R`, regardless
//!   of whether they capture.
//! cites: 27.5
//! expect: run
//! output: 3
//!   15
//! issue: 8141
module Conf;

public class Program
  public static method Make(): () -> int
    var outer := lambda() => do
      return lambda() => 3;
    end;
    return outer();
  end Make;

  public static method Main()
    Console.WriteLine(Program.Make()());
    var total := 0;
    var i := 0;
    while i < 5 do
      total := total + Program.Make()();
      i := i + 1;
    end while;
    Console.WriteLine(total);
  end Main;
end Program;
