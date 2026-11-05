//! case: l4_1803
//! doc: stdlib
//! section: 4
//! title: IList<IShape>.Contains over an interface element type is admitted
//! rule: The members that compare an argument with stored elements by `Equals` are
//!   the equality search: `Contains` and `Remove(item)` of `ICollection<T>`
//! cites: 4.1, 9.2
//! expect: run
//! output: True
//!   False
module Conf;

public interface IShape
  method Area(): int;
end IShape;
class Sq: IShape
  public constructor() end constructor;
  public method Area(): int => 4;
end Sq;
class Program
  public static method Main()
    var a := new Sq();
    var b := new Sq();
    var arr := new Array<IShape>();
    arr.Add(a);
    var lst: IList<IShape> := arr;
    Console.WriteLine(lst.Contains(a));
    Console.WriteLine(lst.Contains(b));
  end Main;
end Program;
