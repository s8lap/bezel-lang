//! case: s28_3_8_7111
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied method returns a lazy query whose lambda captures only a value-typed parameter
//! rule: A composed-frame non-iterator body (`static method Evens(xs: Array<int>): IEnumerable<int> => xs.Where(IsEven);`) is admitted by the same clause
//! cites: 28.3, 22.2, 29.4
//! expect: run
//! stdout: 14
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Big(xs: IEnumerable<int>, limit: int): IEnumerable<int> => xs.Where(lambda(x: int) => x > limit);
  public static method Main()
    var a: Array<int> := [1, 5, 9];
    var s := 0;
    for var x in Big(a, 3) do s := s + x; end;
    Console.WriteLine(s);
  end Main;
end Program;
