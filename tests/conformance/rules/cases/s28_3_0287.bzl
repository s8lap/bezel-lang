//! case: s28_3_0287
//! doc: spec
//! section: 28.3
//! title: an alias read on return and throw paths leaves the owner disposed once
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 469
module Conf;
class Program
  public static method F(i: int): int
    var a: Array<int> := [1,2,3];
    var b := a;
    if i mod 3 = 0 then return b.Count; end;
    if i mod 3 = 1 then throw new Exception("x"); end;
    return a.Count;
  end F;
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      try
        sum := sum + Program.F(i);
      catch Exception => sum := sum + 1;
      end;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
