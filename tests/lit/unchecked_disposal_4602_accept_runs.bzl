// RUN: %bezelc --unchecked-disposal -o %t.exe %s && %t.exe | FileCheck %s
// fix #4602 (proposal 0128 §2.1; spec §28.2 unchecked (release) mode, v0.338):
// `--unchecked-disposal` is accepted (it was "unknown option" before) and the
// program runs to the spec-stated output of conformance s28_6_0036: the
// cascade over an early-disposed field still runs once.
// CHECK: closed
// CHECK-NEXT: released early
// CHECK-NEXT: done
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Resource;

public class Holder
  private owned _m: Resource := new Resource();

  public method Close()
    _m.Dispose();
  end Close;
end Holder;

public class Program
  public static method Main()
    do
      using var h := new Holder();
      h.Close();
      Console.WriteLine("released early");
    end;
    Console.WriteLine("done");
  end Main;
end Program;
