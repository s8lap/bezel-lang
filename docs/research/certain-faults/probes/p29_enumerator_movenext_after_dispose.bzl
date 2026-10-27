module Probe;
public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var e := xs.GetEnumerator();
    e.Dispose();
    Console.WriteLine(e.MoveNext());
  end Main;
end Program;
