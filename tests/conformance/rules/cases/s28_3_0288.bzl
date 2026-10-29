//! case: s28_3_0288
//! doc: spec
//! section: 28.3
//! title: an alias read in a catch arm after a throw leaves the owner disposed once
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 906
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a: Array<int> := [1,2,3];
      var b := a;
      try
        if i mod 2 = 0 then throw new Exception("x"); end;
        sum := sum + b.Count;
      catch Exception => sum := sum + b.Count + a.Count;
      end;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
