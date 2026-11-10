//! case: s20_0901
//! doc: spec
//! section: 20
//! title: marked arm transfer of an Array<int> local: the selected arm moves it, the other arm is fresh
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3, 19.2
//! expect: run
//! output: 10
//! issue: 7907
module Conf;
public class Program
  static method Go(flag: bool): int
    var a := new Array<int>();
    a.Add(1);
    var x := (if flag then move a else new Array<int>());
    return x.Count;
  end Go;

  public static method Main()
    Console.WriteLine($"{Go(true)}{Go(false)}");
  end Main;
end Program;
