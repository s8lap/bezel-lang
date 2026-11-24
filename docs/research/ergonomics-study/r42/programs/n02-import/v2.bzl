module Tools.Import;

import System.IO;

public record Order(Sku: string, Qty: int, Price: decimal);

public static class Orders
  // a decoded error: returned as a value, not thrown
  public static method ParseLine(line: string, n: int): Result<Order, string>
    var f := line.Split(',');
    if f.Count <> 3 then
      return Error($"line {n}: expected 3 fields");
    end;
    var qty := int.ParseOrNull(f[1]);
    if qty = null then
      return Error($"line {n}: bad qty '{f[1]}'");
    end;
    var price := decimal.ParseOrNull(f[2]);
    if price = null then
      return Error($"line {n}: bad price '{f[2]}'");
    end;
    return Ok(new Order(f[0], qty, price));
  end ParseLine;
end Orders;

public class Program
  public static method Main()
    const dir := "/var/tmp/bezel-scratch/ergo2/run/n02";
    Directory.CreateDirectory(dir);
    File.WriteAllText(Path.Combine(dir, "a.csv"), "A1,2,1.50\nA2,x,3.00\nA3,1,4.00\n");
    File.WriteAllText(Path.Combine(dir, "b.csv"), "B1,1,10.00\nB2,2,5.00\nB3,1,14.50\n");
    File.WriteAllText(Path.Combine(dir, "c.csv"), "C1;2;3\n");
    var files: Array<string> := ["a.csv", "b.csv", "c.csv", "missing.csv"];
    var nFiles := 0;
    var nOrders := 0;
    var value := 0m;
    for var name in files do
      // r42: a missing file reads as empty instead of raising FileNotFoundException (#7057),
      // so the documented catch arm never runs; pre-check (the doc's discouraged shape)
      if not File.Exists(Path.Combine(dir, name)) then
        Console.WriteLine($"{name}: not found");
        continue;
      end;
      try
        using var lines := File.ReadAllLines(Path.Combine(dir, name));
        nFiles++;
        var ok := 0;
        var bad := 0;
        var firstBad: string? := null;
        var n := 0;
        for var line in lines do
          n++;
          case Orders.ParseLine(line, n) of
            Ok(var o) => do
              ok++;
              value += o.Qty * o.Price;
            end,
            Error(var m) => do
              bad++;
              firstBad ??= m;
            end
          end;
        end;
        nOrders += ok;
        var detail := (if firstBad <> null then $" ({firstBad})" else "");
        Console.WriteLine($"{name}: {ok} ok, {bad} bad{detail}");
      catch
        FileNotFoundException => Console.WriteLine($"{name}: not found")
      end try;
    end;
    Console.WriteLine($"total {nFiles} files, {nOrders} orders, value {value:F2}");
    Directory.Delete(dir, recursive := true);
  end Main;
end Program;
