var best: Res? := null;
for var i in 0..<count do
    var cur := new Res(i);
    if best = null or cur.Score() > best!.Score() then
        best := move cur;          // transfer; the displaced best is disposed (§28.7)
    else
        cur.Dispose();             // or let cur auto-drop with the must-use warning
    end;
    // after the if: cur is moved-from on the taken path, disposed on the other —
    // the §28.3 join rules (maybe-consumed) forbid reading it; scope-end disposal is per-path.
end;
// best: owned; discharge or auto-drop per the standard rules.
