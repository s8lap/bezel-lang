public class ChannelClosedException: Exception
end ChannelClosedException;

public class ChannelFullException: Exception        // v0.174: sync Send on a full bounded channel
end ChannelFullException;
