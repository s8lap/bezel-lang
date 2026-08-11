var fresh := Connect("fresh");   // build the replacement first — it may throw
using var old := move pool[i];   // then take — nothing throwable follows
pool[i] := fresh;                // restore the slot — required on every exit path
