//! case: s28_9_4603
//! doc: spec
//! section: 28.9
//! title: a string reached through an `as` test and stored in an Object field stays valid
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 4.6, 12.7
//! expect: run
//! stdout: 690
module Conf;

public class Holder
  public O: Object := "";
  public method Put(o: Object)
    this.O := o;
  end Put;
  public method Put2(n: int, o: Object)
    this.O := o;
  end Put2;
  public method PutAsStr(o: Object)
    var s := o as string;
    if s <> null then
      this.O := s;
    end;
  end PutAsStr;
end Holder;

public class Program
  public static method Make(i: int): string => "s" + i.ToString();
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      var h := new Holder();
      do
        var x := Make(i);
        h.PutAsStr(x);
      end;
      var junk := Make(99999);
      total := total + h.O.ToString().Length + junk.Length - 6;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
