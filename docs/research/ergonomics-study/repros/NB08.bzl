// bug: NB8
// title: spurious internal warning on a short-circuit operand in a generic body
// issue: new (the message cites closed #2805)
// rests-on: spec §17.1: `and` evaluates its right operand when the left is true
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: quiet
// stdout: True
module P;
public class Box<T> where T: IComparable<T>
  private owned _xs: Array<T> := new Array<T>();
  public method Add(x: T) => _xs.Add(x);
  public method FirstLess(i: int, j: int): bool => i < _xs.Count and _xs[i].CompareTo(_xs[j]) < 0;
end Box;
public class Program
  public static method Main()
    using var b := new Box<int>();
    b.Add(1); b.Add(2);
    Console.WriteLine(b.FirstLess(0, 1));
  end Main;
end Program;
