module Query.Inventory;

record Txn(Item: string, Delta: int);

public class Program
  public static method Main()
    var stock: Dictionary<string, int> := ["bolt": 10, "nut": 5];
    var txns: Array<Txn> := [new Txn("bolt", -3), new Txn("nut", -7), new Txn("washer", 4), new Txn("bolt", -8)];
    for var t in txns do
      if not stock.ContainsKey(t.Item) then
        stock[t.Item] := 0;
      end;
      stock[t.Item] += t.Delta;
    end;
    var names := stock.Keys.ToArray();
    names.Sort();
    for var n in names do
      Console.WriteLine($"{n} {stock[n]}");
    end;
    var negative := names.Where(lambda(n) => stock[n] < 0);
    Console.WriteLine($"negative: {String.Join(", ", negative)}");
  end Main;
end Program;
