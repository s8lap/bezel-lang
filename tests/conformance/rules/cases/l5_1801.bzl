//! case: l5_1801
//! doc: stdlib
//! section: 5
//! title: IReadOnlyList<T>.Contains is refused at a function-typed element (hashable block)
//! rule: The six operators that require **hashable** `T` are declared in their own
//!   constrained block.
//! cites: 4.1
//! expect: reject
//! diagnostic: hashable
module Conf;

class Program
  public static method Main()
    var f: (int) -> int := lambda(x: int) => x + 1;
    var arr := new Array<(int) -> int>();
    arr.Add(f);
    var rol: IReadOnlyList<(int) -> int> := arr;
    Console.WriteLine(rol.Contains(f));
  end Main;
end Program;
