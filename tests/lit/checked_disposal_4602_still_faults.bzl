// RUN: %bezelc --checked-disposal -o %t.exe %s && %t.exe | FileCheck %s
// fix #4602 (spec §28.2 checked mode, v0.338): `--checked-disposal` stays
// accepted and checked. A read through a surviving borrow after Dispose raises
// DisposedAccessException (conformance s28_2_0011 shape).
// CHECK: faulted
module Conf;

public class Widget
  public property Label: string;

  public constructor()
    Label := "live";
  end constructor;

  destructor
  end destructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    var view := w;
    w.Dispose();
    try
      Console.WriteLine(view.Label);
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Main;
end Program;
