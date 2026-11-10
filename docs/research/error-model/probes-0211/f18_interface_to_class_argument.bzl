// Probe f18 (proposal 0211 appendix, compiler gap): r40 accepts an interface-typed
// value where a class-typed parameter is declared. Spec §5.1 lists no conversion
// from an interface to a class, so both calls below should be rejected. The second
// passes an object that is not an `Exception` at all and reads its `Message`.
// The proposal does not rely on this acceptance.
module F18;

public interface IMark
end IMark;

public class A: Exception, IMark
  public constructor(m: string) base(m); end constructor;
end A;

public class NotAnException: IMark
end NotAnException;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"Log: '{ex.Message}'");
  end Log;

  public static method Main()
    using var a := new A("hello");
    var i: IMark := a;
    Program.Log(i);
    using var n := new NotAnException();
    var j: IMark := n;
    Program.Log(j);
  end Main;
end Program;
