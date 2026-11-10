module Interop.Libc;

extern "libc"
  public method strlen(s: CString): UIntPtr;
  public method abs(n: int): int;
  public method getpid(): int;
  public method toupper(c: int): int;
end;

public class Program
  public static method Main()
    using var s := CString.From("hello");
    Console.WriteLine($"strlen {strlen(s)}");
    Console.WriteLine($"abs {abs(-7)}");
    Console.WriteLine((if getpid() > 0 then "pid ok" else "pid bad"));
    var sb := new StringBuilder();
    for var c in "bezel" do
      sb.Append(char(toupper(int(c))));
    end;
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
