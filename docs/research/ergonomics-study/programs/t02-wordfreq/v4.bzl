module Tools.WordFreq;

record WordCount(Key: string, Value: int);

public class Program
  public static method Main()
    var sbAll := new StringBuilder();
    var l := Console.ReadLine();
    while l <> null do
      sbAll.Append(l).Append('\n');
      l := Console.ReadLine();
    end;
    var text := sbAll.ToString().ToLower();
    var counts := new Dictionary<string, int>();
    var sb := new StringBuilder();
    for var c in text do
      if Char.IsLetter(c) then
        sb.Append(c);
      elif sb.Length > 0 then
        var w := sb.ToString();
        counts[w] := (counts.GetOrNull(w) ?? 0) + 1;
        sb.Clear();
      end;
    end;
    if sb.Length > 0 then
      var w := sb.ToString();
      counts[w] := (counts.GetOrNull(w) ?? 0) + 1;
    end;
    // OrderBy/ThenBy over a string key do not sort on r40 (compiler bug); sort by hand.
    // Array<DictionaryEntry<..>> does not sort on r40 (compiler bug); copy into our own record.
    var entries := new Array<WordCount>();
    for var e in counts do
      entries.Add(new WordCount(e.Key, e.Value));
    end;
    entries.Sort(lambda(x, y) => (if x.Value <> y.Value then y.Value - x.Value else x.Key.CompareTo(y.Key)));
    for var e in entries.Take(5) do
      Console.WriteLine($"{e.Key} {e.Value}");
    end;
  end Main;
end Program;
