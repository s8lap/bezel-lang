//! case: s14_2_0080
//! doc: spec
//! section: 14.2
//! title: a one-argument OwningBuffer over an enum with a zero-valued constant runs
//! rule: The value presentation's one-argument constructor initializes every slot to default(T); default(T) is defined for an enum that declares a constant of underlying value 0 (17.11).
//! cites: 14.2, 17.11
//! expect: run
//! stdout: 4
//! issue: 5821
module Conf;

public enum Level None := 0, High := 2; end Level;

public class Program
  public static method Main()
    using var b := new OwningBuffer<Level>(4);
    Console.WriteLine(b.Length);
  end Main;
end Program;
