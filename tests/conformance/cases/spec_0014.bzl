var user: User? := GetUser();
if user <> null then
  Console.WriteLine(user.Name);     // narrowed to User here
end;
