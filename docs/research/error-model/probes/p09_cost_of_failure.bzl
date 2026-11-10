// Probe p09: the cost of one expected failure on each tier, r40, default flags.
// Each loop performs N failing lookups/parses and counts the failures.
// The throw/catch loop is what a naive `try? f()` desugaring would cost.
module P09;

import System.Diagnostics;

public class Program
  public static method Depth(n: int, s: string): int
    if n = 0 then
      return int.Parse(s);
    end if;
    return Program.Depth(n - 1, s) + 0;
  end Depth;

  public static method Main()
    const N: int := 2000000;
    var d := new Dictionary<int, int>();
    d.Add(1, 1);
    var misses := 0;

    using var sw := Stopwatch.StartNew();
    for var i in 0..<N do
      if not int.TryParse("12x", out var v) then misses += 1; end if;
    end for;
    Console.WriteLine($"TryParse failure      : {sw.ElapsedMilliseconds} ms for {misses}");

    misses := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      try
        _ := int.Parse("12x");
      catch
        FormatException => do misses += 1; end,
      end try;
    end for;
    Console.WriteLine($"Parse + catch         : {sw.ElapsedMilliseconds} ms for {misses}");

    misses := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      try
        _ := Program.Depth(8, "12x");
      catch
        FormatException => do misses += 1; end,
      end try;
    end for;
    Console.WriteLine($"Parse + catch, 8 frames: {sw.ElapsedMilliseconds} ms for {misses}");

    misses := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      if not d.TryGetValue(i + 2, out var v) then misses += 1; end if;
    end for;
    Console.WriteLine($"TryGetValue miss      : {sw.ElapsedMilliseconds} ms for {misses}");

    misses := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      if d.GetOrNull(i + 2) = null then misses += 1; end if;
    end for;
    Console.WriteLine($"GetOrNull miss        : {sw.ElapsedMilliseconds} ms for {misses}");

    misses := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      try
        _ := d[i + 2];
      catch
        KeyNotFoundException => do misses += 1; end,
      end try;
    end for;
    Console.WriteLine($"indexer + catch       : {sw.ElapsedMilliseconds} ms for {misses}");

    // Success paths, for the baseline.
    var hits := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      if int.TryParse("123", out var v) then hits += 1; end if;
    end for;
    Console.WriteLine($"TryParse success      : {sw.ElapsedMilliseconds} ms for {hits}");
    hits := 0; sw.Reset(); sw.Start();
    for var i in 0..<N do
      try
        _ := int.Parse("123");
        hits += 1;
      catch
        FormatException => do misses += 1; end,
      end try;
    end for;
    Console.WriteLine($"Parse in try, success : {sw.ElapsedMilliseconds} ms for {hits}");
  end Main;
end Program;
