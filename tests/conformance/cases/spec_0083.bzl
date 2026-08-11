var r: Range<int> := (if someCondition then 1..10 else 10..);
for var i in r do ... end;        // InvalidOperationException if r is open-ended
