//! case: s27_3_0913
//! doc: spec
//! section: 27.3
//! title: a long lambda body at a call argument whose substituted slot is int is rejected
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 14.5
//! expect: reject
//! diagnostic: implicit
//! issue: 8168
module Conf;

public class Program
  public static method Apply<T>(f: (int) -> T, v: int): T
    return f(v);
  end Apply;

  public static method Main()
    Console.WriteLine(Program.Apply<int>(lambda(x) => x * 3L, 2));
  end Main;
end Program;
