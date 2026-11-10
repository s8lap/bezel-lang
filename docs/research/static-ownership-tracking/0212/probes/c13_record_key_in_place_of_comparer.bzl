module Probe;
// CRITIQUE ROUND 2, NOTE 1 (the comparer rule is narrow), the rewrite to teach: the key is wrapped in a record that declares its own equality, so the dictionary needs no comparer object at all — the rewrite `SortedDictionary` already requires for an ordering (stdlib §4.17). EXPECT: compiles with no warning and no report line; clean in both modes; prints True. Under 0212: unchanged; `Dictionary<FoldedKey, int>` is not a carrier type and stays sendable.
public record FoldedKey(Value: string)
  public method Equals(other: FoldedKey): bool => Value.ToUpper() = other.Value.ToUpper();
  public override method GetHashCode(): int => Value.ToUpper().GetHashCode();
end FoldedKey;
public class Program
  public static method Main(): int
    using var d := new Dictionary<FoldedKey, int>();
    d.Add(new FoldedKey("Ann"), 1);
    Console.WriteLine(d.ContainsKey(new FoldedKey("ANN")));
    Console.WriteLine(d[new FoldedKey("ann")]);
    return 0;
  end Main;
end Program;
