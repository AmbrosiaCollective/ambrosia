# Native queue callbacks

Native workers start with no guest TLS, runtime scope or module-catalog access.
Capture an adapter with `Runtime::capture_callback` while the correct initialized
guest context is active, then schedule that adapter on the runtime's joined
workers. Calling a raw guest pointer from a native queue job is insufficient.

The adapter retains a shared module-graph facade and a generation-pinned thread
snapshot. Each invocation establishes its diagnostic stack, runtime/catalog
scope, fresh guest TLS, module thread notifications and exit cleanup. It refuses
execution on the process owner thread. Host pool threads may be reused; persistent
guest TLS across separate callback invocations is not promised. Capture fails
without an active guest context and an initialized module execution.

The runtime reference and callback context are borrowed. The registration owner
must retain its adapter and context until queued/in-flight use ends. Invoke it
only on this runtime's workers, and call `Runtime::shutdown_workers` before
closing the process execution or destroying service objects. Native command's
scope guard enforces this ordering, including exceptional exits. Runtime's
destructor also joins workers before member destruction. Queue shutdown cancels
pending timers, joins active callbacks and refuses new queues/work; it is terminal.

This is native Linux callback infrastructure. It does not register package,
user or input notifications by itself. Each service must implement bounded
registration, cancellation, unsubscribe/wait behavior and delivery from a real
event or state change. Capturing an adapter does not prove that the game has
received a callback or reached an interactive menu. Other host backends have not
been verified.
