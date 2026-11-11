//! case: s28_3_4602
//! doc: spec
//! section: 28.3
//! title: reassigning a collection-literal owned local from a borrowed parameter is rejected
//! rule: an **owned-state** LHS requires an owned RHS
//! cites: 28.3.4, 15.4
//! expect: reject
//! diagnostic: owned
module Conf;

public class Program
  public static method Make(a: Array<int>): owned Array<int>
    var d: Array<int> := [1, 2, 3];
    d := a;
    return d;
  end Make;
  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
