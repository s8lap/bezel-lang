using var j1 := Domain.Spawn(lambda() => Index(repo));   // ERROR: entry captures repo
using var j2 := Domain.Spawn(Work, move sessions);       // ERROR when sessions is a
                                                         //   Dictionary<string, Session>:
                                                         //   borrowing slots of class
                                                         //   elements are not sendable
