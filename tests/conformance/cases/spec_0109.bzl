var name: string? := null;
Console.WriteLine($"user={name}");     // prints: user=null
var x: double? := null;
Console.WriteLine($"[{x,8:F2}]");      // prints: [    null] — the type-formattability check
                                       //   applies to double; F2 is not invoked on null;
                                       //   the alignment pads the text
x := 3.0;
Console.WriteLine($"[{x,8:F2}]");      // prints: [    3.00] — present value: ordinary dispatch
