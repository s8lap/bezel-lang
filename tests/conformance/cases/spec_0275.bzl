using var inbox := new Channel<Report>();          // unbounded; this domain receives
using var worker := Domain.Spawn(ProcessAll, inbox.NewSender());   // sender moves to the new domain

// in this domain:
using var first := await inbox.Receive();          // ownership of one Report transfers in
await worker;                                      // join: a fault in the spawned domain
                                                   //   surfaces here as DomainFaultedException (§30.14)

// in the spawned domain (static method):
public static method ProcessAll(owned tx: ChannelSender<Report>)
  using var sender := move tx;                     // marker: adopt the owned parameter
  sender.Send(BuildReport(1));                     // ownership of each Report transfers out
  sender.Send(BuildReport(2));
end ProcessAll;                                    // sender disposed: last sender closes the channel
