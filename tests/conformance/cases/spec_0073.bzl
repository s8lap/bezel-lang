var r: Range<int> := 1..10 if someCondition else 10..;
for var i in r do ... end;        // InvalidOperationException if r is open-ended
