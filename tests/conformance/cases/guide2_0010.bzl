method Find(key: string): User?
  // ... returns null if absent
end Find;

var u := directory.Find("alice");
if u <> null then
  Greet(u);                        // narrowed
else
  Greet(defaultUser);
end;
