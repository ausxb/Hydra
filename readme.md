# Hydra

Hydra is a server framework for the Windows API based on IO completion ports.
The server template class manages the connection lifecycle and events on the completion port
using callbacks implemented as members of an application class. The class implementing
the application and callbacks is passed as the template argument of the server class. 
Several parameters related to network setup and resource allocation
are configurable on construction of the server.

#### Changes
- Better worker synchronization with synchronization barriers.
	Allows more controlled shutdown. Eliminates burden on workers
	to be aware of concurrent shutdown and unnecessary logic in event loop.
- Multi-phase shutdown with timeout for graceful connection termination.
	Timeout is now per-worker and is based on the time from the last arrived packet.
	It is based on a call to GetQueuedCompletionStatus() with a timeout set, so each
	time a new completion packet is dequeued, the timeout resets.

#### Future Revisions
- **Applications should delete packets on CLOSE error. Server should delete packets on ACCEPT error.**
	Update notes on callbacks accordingly. Packets currently not cleaned up in server, so will leak.
- **Move heap outside of server. Have application supply allocator.**
- Better error interface? Use struct and record source of internal errors.
- Per-connection timeouts?
- More documentation
- UDPServer
- Thread load balancing? Use timing based QPC to determine when to add or remove threads.
	Mutex contention &mdash; maybe associate new connections with mutexes
	in a manner that reduces contention, some connections may be more frequent and demanding.