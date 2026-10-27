module Probe;
public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x) => x > 1);
    var n := q.Count();
    for var x in q do Console.WriteLine(x); end;
    Console.WriteLine(n);
  end Main;
end Program;
