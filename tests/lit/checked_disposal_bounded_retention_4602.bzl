// RUN: %bezelc --checked-disposal -o %t.exe %s && %t.exe | FileCheck %s --check-prefix=BOUNDED
// RUN: env BEZEL_CHECKED_DISPOSAL_QUARANTINE_ALL=1 %t.exe | FileCheck %s --check-prefix=QUARANTINED
// #4602 (spec §28.2 checked mode, v0.338): checked disposal may defer the
// reclamation of a disposed block (quarantine) so a surviving borrow's member
// access can read its disposed flag — it need not keep a block no borrow can
// reach. 2,000,000 create-and-dispose cycles of a destructor class, each a
// fresh local released at its scope end, used only as a receiver: the blocks
// are freed and the peak RSS stays bounded (MEASURED ~4 MB; quarantined, the
// same loop holds ~63 MB). The test-only BEZEL_CHECKED_DISPOSAL_QUARANTINE_ALL
// knob restores the full quarantine, which is what the second RUN line pins:
// the probe itself can tell the two apart.
// BOUNDED: done 5999995
// BOUNDED-NEXT: bounded
// QUARANTINED: done 5999995
// QUARANTINED-NEXT: unbounded
module Probe;

public class Widget
  public property Value: int;

  public constructor(v: int)
    Value := v mod 7;
  end constructor;

  destructor
  end destructor;
end Widget;

public class Program
  public static method PeakKb(): int
    using var fs := new FileStream("/proc/self/status", FileMode.Open, FileAccess.Read);
    var buf := new Array<byte>(16384, 0);
    var n := fs.Read(buf, 0, buf.Count);
    var key := "VmHWM:";
    var i := 0;
    while i + 6 < n do
      var m := 0;
      while m < 6 and char(buf[i + m]) = key[m] do
        m := m + 1;
      end while;
      if m = 6 then
        var j := i + 6;
        var v := 0;
        while j < n and char(buf[j]) <> 'k' do
          if buf[j] >= 48 and buf[j] <= 57 then
            v := v * 10 + (int(buf[j]) - 48);
          end if;
          j := j + 1;
        end while;
        return v;
      end if;
      i := i + 1;
    end while;
    return -1;
  end PeakKb;

  public static method Main()
    var total := 0;
    var i := 0;
    while i < 2000000 do
      var w := new Widget(i);
      total := total + w.Value;
      i := i + 1;
    end while;
    Console.WriteLine($"done {total}");
    var kb := PeakKb();
    if kb > 0 and kb < 32768 then
      Console.WriteLine("bounded");
    else
      Console.WriteLine($"unbounded {kb}");
    end if;
  end Main;
end Program;
