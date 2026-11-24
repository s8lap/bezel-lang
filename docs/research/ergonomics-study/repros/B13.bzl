// bug: B13
// title: `Sum` with a `decimal` selector returns a wrong value
// issue: #8388
// rests-on: stdlib §5 `Sum<U>(selector)`; stdlib §9.3: scale is not part of a decimal value, so 7.00m renders as 7
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 7
// stdout: 4
// stdout: 3
module P;
record Item(Qty: int, Price: decimal);
public class Program
  public static method Main()
    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)];
    Console.WriteLine(items.Sum(lambda(i) => i.Qty * i.Price));
    var ds: Array<decimal> := [1.5m, 2.5m];
    Console.WriteLine(ds.Sum());
    Console.WriteLine(items.Sum(lambda(i) => i.Qty));
  end Main;
end Program;
