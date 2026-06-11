using var conn := new Connection("db://localhost");  // owned, disposed at block end
var view := conn.View();                             // borrowed, disposes nothing
