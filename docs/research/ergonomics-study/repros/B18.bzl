// bug: B18
// title: Internal warning on `ToString()` over a type parameter
// issue: none filed
// rests-on: spec §8.1: `ToString` is universal; a generic body may call it on `T`
// source: study program programs/t29-partition/v2.bzl, verbatim below this header
// note: The study program the entry names, copied verbatim.
// expect: quiet
// stdout: even: 2 4 6 8 10
// stdout: odd: 1 3 5 7 9
// stdout: long: tree green
// stdout: short: a is sky
module Generic.Partition;

public record Parts<T>(Yes: Array<T>, No: Array<T>);

public static class Seq
  public static method Partition<T>(xs: IEnumerable<T>, pred: (T) -> bool, out yes: Array<T>, out no: Array<T>)
    yes := new Array<T>();
    no := new Array<T>();
    for var x in xs do
      if pred(x) then yes.Add(x); else no.Add(x); end;
    end;
  end Partition;
end Seq;

public class Program
  private static method Join<T>(xs: Array<T>): string => String.Join(" ", xs.Select(lambda(x) => x.ToString()));

  public static method Main()
    Seq.Partition<int>(1..10, lambda(n) => n mod 2 = 0, out var even, out var odd);
    Console.WriteLine($"even: {Join(even)}");
    Console.WriteLine($"odd: {Join(odd)}");
    var words: Array<string> := ["a", "tree", "is", "green", "sky"];
    Seq.Partition<string>(words, lambda(w) => w.Length > 3, out var long, out var short);
    Console.WriteLine($"long: {Join(long)}");
    Console.WriteLine($"short: {Join(short)}");
  end Main;
end Program;
