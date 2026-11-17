//! case: s28_9_4715
//! doc: spec
//! section: 28.9
//! title: a string written via an out parameter into an Object field is readable after its scope ends
//! rule: a `using var`, a discard or scope end over such a value disposes nothing; the type's
//!   lifetime is managed by the runtime without user involvement
//! cites: 28.9, 8.6, 4.6
//! expect: run
//! stdout: 400
//! issue: 8598
module Conf;

public class Holder
  public O: Object := "";

  public static method Fill(n: int, out r: string): bool
    r := Make.Str(n);
    return true;
  end Fill;

  public method M()
    Holder.Fill(7, out var s);
    var o: Object := "";
    o := s;
    this.O := o;
  end M;
end Holder;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public class Program
  public static method Main()
    var i := 0;
    var total := 0;
    while i < 200 do
      var h := new Holder();
      h.M();
      var junk := Make.Str(99999);
      total := total + h.O.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
