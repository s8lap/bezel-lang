module Query.Histogram;

public class Program
  public static method Main()
    const text := "a bb ccc dd e ffff ggg hh i jjjjj";
    var hist := new SortedDictionary<int, int>();
    for var w in text.Split(' ') do
      hist[w.Length] := (hist.GetOrNull(w.Length) ?? 0) + 1;
    end;
    for var e in hist do
      Console.WriteLine($"{e.Key} | {String.Repeat('*', e.Value)}");
    end;
  end Main;
end Program;
