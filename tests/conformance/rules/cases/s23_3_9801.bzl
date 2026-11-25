//! case: s23_3_9801
//! doc: spec
//! section: 23.3
//! title: A block-bodied static int and string getter is invoked on every read, 200x
//! rule: Each accessor uses `=> expression` or `=> do ... end`.
//! cites: 23.3, 22.12
//! expect: run
//! output: 1400
//!   hi!
//!   3
//! notes: Pins #9224 (pick 08508faec): a read of a block-bodied static property must invoke a receiver-
//!   less getter. 200 reads of N add 7 each: 1400; S is hi! and its Length is 3.
module Conf;

public class H
  public static property N: int
    get => do return 7; end;
  end N;
  public static property S: string
    get => do return "hi" + "!"; end;
  end S;
end H;

public class Program
  public static method Main(): int
    var t := 0;
    var i := 0;
    while i < 200 do
      t := t + H.N;
      i := i + 1;
    end while;
    Console.WriteLine(t);
    Console.WriteLine(H.S);
    var s := H.S;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
end Program;
