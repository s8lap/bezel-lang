module SmokeFormat;

// decimal, double, DateTime and integer formatting (musl's libc and libc++'s
// locale-free paths).
public class Program
  public static method Main()
    var d := 1234.5678m;
    var x := 3.14159;
    Console.WriteLine($"{d:F2} {d.Round(1)} {x:F3} {x:E2} {x:P1}");
    var dt := new DateTime(2024, 3, 5, 14, 7, 9);
    Console.WriteLine(dt.ToString("yyyy-MM-dd HH:mm:ss"));
    var n := 255;
    Console.WriteLine($"{n:X} {n:B} {n:D5}");
  end Main;
end Program;
