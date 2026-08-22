new Connection("a")            new()                 // target-typed shorthand
new Point(1, 2) with(X := 5)                         // `with` = record copy-with-overrides
obj.Method<int>(args)          Registry<int>.Register()
user?.Name?.Length             handler?(42)          // null-conditional access / call
x!                                                   // postfix null-forgiving
move x     move obj._field     move arr[i]           // the use-site transfer marker (§28.8.1)
nameof(amount)   typeof(BankAccount)   default(int)
int.Parse(s)   int.MaxValue   short(x)   long?(n)    // primitive keyword in expression position; casts
this   base.Speak()
