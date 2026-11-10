module Model.Money;

public record Money(Amount: decimal, Currency: string)
  public static method operator +(a: Money, b: Money): Money
    if a.Currency <> b.Currency then
      throw new InvalidOperationException($"currency mismatch: {a.Currency} vs {b.Currency}");
    end;
    return new Money(a.Amount + b.Amount, a.Currency);
  end operator +;

  public override method ToString(): string => $"{Amount:F2} {Currency}";
end Money;

public class Program
  public static method Main()
    var a := new Money(12.50m, "USD");
    var b := new Money(7.25m, "USD");
    Console.WriteLine($"{a} + {b} = {a + b}");
    var price := a with(Amount := 3.00m);
    Console.WriteLine($"price with amount 3.00 -> {price}");
    try
      var bad := a + new Money(1m, "EUR");
      Console.WriteLine(bad);
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end;
    Console.WriteLine($"equal: {new Money(1.0m, "USD") = new Money(1.00m, "USD")}");
  end Main;
end Program;
