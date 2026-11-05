//! case: l4_1802
//! doc: stdlib
//! section: 4
//! title: IList<K>.Contains over a class element compares by the class's Equals
//! rule: The members that compare an argument with stored elements by `Equals` are
//!   the equality search: `Contains` and `Remove(item)` of `ICollection<T>`
//! cites: 4.1, 9.2
//! expect: run
//! output: True
//!   False
module Conf;

class K
  public constructor() end constructor;
end K;
class Program
  public static method Main()
    var a := new K();
    var b := new K();
    var arr := new Array<K>();
    arr.Add(a);
    var lst: IList<K> := arr;
    Console.WriteLine(lst.Contains(a));
    Console.WriteLine(lst.Contains(b));
  end Main;
end Program;
