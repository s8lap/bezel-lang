//! case: s27_3_0914
//! doc: spec
//! section: 27.3
//! title: an int lambda body at a call argument whose substituted slot is long widens
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 14.5, 5.1
//! expect: run
//! output: 6
//! issue: 8168
module Conf;

public class Program
  public static method Apply<T>(f: (int) -> T, v: int): T
    return f(v);
  end Apply;

  public static method Main()
    Console.WriteLine(Program.Apply<long>(lambda(x) => x * 3, 2));
  end Main;
end Program;
