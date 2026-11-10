//! case: s28_3_1402
//! doc: spec
//! section: 28.3
//! title: a second marked arm transfer of the same collection local is rejected (maybe-consumed)
//! rule: unreadable after the join, no flow around a back edge into a use
//! cites: 20, 28.3 K7.4
//! expect: reject
//! diagnostic: maybe-consumed
module Conf;
public class Program
  static method Go(flag: bool): int
    var a := new Array<int>();
    a.Add(1);
    var x := (if flag then move a else new Array<int>());
    var y := (if flag then move a else new Array<int>());
    return x.Count + y.Count;
  end Go;

  public static method Main()
    Console.WriteLine(Go(true));
  end Main;
end Program;
