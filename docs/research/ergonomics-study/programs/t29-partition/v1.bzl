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
  private static method Join<T>(xs: Array<T>): string => String.Join(" ", xs.Select(lambda(x) => $"{x}"));

  public static method Main()
    Seq.Partition(1..10, lambda(n) => n mod 2 = 0, out var even, out var odd);
    Console.WriteLine($"even: {Join(even)}");
    Console.WriteLine($"odd: {Join(odd)}");
    var words: Array<string> := ["a", "tree", "is", "green", "sky"];
    Seq.Partition(words, lambda(w) => w.Length > 3, out var long, out var short);
    Console.WriteLine($"long: {Join(long)}");
    Console.WriteLine($"short: {Join(short)}");
  end Main;
end Program;
