method TryGet(k: string, out v: Conn?): bool;
method TryGet(k: string, out owned v: Conn?): bool;   // ERROR at the second declaration
seen.Add(candidate);                     // ERROR when Add takes `owned`: write Add(move candidate)
r.Append(move new Widget());             // ERROR: a fresh producer announces itself
return move fresh;                       // ERROR: `return` under `owned` is target-announced
