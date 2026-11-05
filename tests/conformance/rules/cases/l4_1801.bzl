//! case: l4_1801
//! doc: stdlib
//! section: 4
//! title: IList<int>.Contains finds an element by the equality search
//! rule: The members that compare an argument with stored elements by `Equals` are
//!   the equality search: `Contains` and `Remove(item)` of `ICollection<T>`
//! cites: 4.1, 9.2
//! expect: run
//! output: True
//!   False
module Conf;

class Program
  public static method Main()
    var arr := new Array<int>();
    arr.Add(3);
    var lst: IList<int> := arr;
    Console.WriteLine(lst.Contains(3));
    Console.WriteLine(lst.Contains(4));
  end Main;
end Program;
