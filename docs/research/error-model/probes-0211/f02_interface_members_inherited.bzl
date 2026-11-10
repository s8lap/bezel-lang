// Probe f02 (proposal 0211 §3): may an interface member be satisfied by a member
// the class INHERITS? Spec §14.2 (interface conformance, rule 1): "Implementation
// may be direct (the class declares the matching members), inherited (a base
// class declares them), or compiler-generated".
// Part 1: a user base class declares the property; the derived class lists the
// interface and declares nothing.
module F02;

public interface IReport
  readonly property Message: string;
end IReport;

public class Base
  private _m: string;
  public constructor(m: string) _m := m; end constructor;
  public property Message: string => _m;
end Base;

public class Derived: Base, IReport
  public constructor(m: string) base(m); end constructor;
end Derived;

public class Program
  public static method Main()
    using var d := new Derived("from the base class");
    var r: IReport := d;
    Console.WriteLine($"inherited member through the interface: {r.Message}");
  end Main;
end Program;
