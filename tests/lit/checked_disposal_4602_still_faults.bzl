// RUN: %bezelc --checked-disposal -o %t.exe %s && %t.exe | FileCheck %s
// fix #4602 (spec §28.2 checked mode, v0.338): `--checked-disposal` stays
// accepted and checked. A read through a surviving borrow after Dispose raises
// DisposedAccessException (conformance s28_2_0011 shape).
// Re-cut by #7435 (§28.2, v0.357): the kill is conditional -- a read through
// an alias that the disposal dominates on every path is now §28.2's compile
// error, and one it reaches on only some paths is the runtime backstop this
// file pins.
// CHECK: faulted
// CHECK-NEXT: live
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
  public static method Run(kill: bool)
    var w := new Widget();
    var view := w;
    if kill then
      w.Dispose();
    end;
    try
      Console.WriteLine(view.Label);
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Run;

  public static method Main()
    Run(true);
    Run(false);
  end Main;
end Program;
