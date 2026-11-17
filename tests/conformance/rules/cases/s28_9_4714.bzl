//! case: s28_9_4714
//! doc: spec
//! section: 28.9
//! title: an async method that holds a string in an Object local across an await reads it intact
//! rule: a `using var`, a discard or scope end over such a value disposes nothing; the type's
//!   lifetime is managed by the runtime without user involvement
//! cites: 28.9, 8.6, 4.6
//! expect: run
//! stdout: 690
//! issue: 8598
module Conf;

public class Holder
  public async method M(x: string): Task<int>
    var o: Object := "";
    o := x;
    await Task.Delay(1);
    return o.ToString().Length;
  end M;
end Holder;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public class Program
  public static async method Main(): Task<int>
    var i := 0;
    var total := 0;
    while i < 200 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        total := total + await h.M(x);
      end;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
