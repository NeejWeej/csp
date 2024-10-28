from datetime import timedelta
from typing import Dict

from csp.impl.enum import Enum
from csp.impl.struct import Struct

CSP_AUTOGEN_HINTS = {"cpp_header": "csp/adapters/websocket/websocket_types.h"}


class WebsocketStatus(Enum):
    ACTIVE = 0
    GENERIC_ERROR = 1
    CONNECTION_FAILED = 2
    CLOSED = 3
    MESSAGE_SEND_FAIL = 4


class ActionType(Enum):
    CONNECT = 0
    DISCONNECT = 1
    PING = 2


class WebsocketHeaderUpdate(Struct):
    key: str
    value: str


class ConnectionRequest(Struct):
    uri: str
    action: ActionType  # Connect, Disconnect, Ping, etc
    # Whetehr we maintain the connection
    persistent: bool = True  # Only relevant for Connect requests
    reconnect_interval: timedelta = timedelta(seconds=2)
    on_connect_payload: str  # message to send on connect
    headers: Dict[str, str] = {}


"""
```markdown
# WebSocket Interface Requirements

## Overview
This interface provides a high-level abstraction for WebSocket connections, 
offering 'subscribe' and 'send' functionalities. It is designed to handle 
various connection scenarios with flexible behavior based on the provided 
configuration.

## Core Components

### ConnectionRequest Structure
```cpp
class ConnectionRequest {
public:
    std::string url;
    ActionType action;  // Enum: Connect, Disconnect, Ping, etc.
    bool persistent;
    std::any on_connect_payload;
    // Additional fields as needed
};
```

### Main Interface Functions
1. `subscribe(const ConnectionRequest& request)`
2. `send(const ConnectionRequest& request)`

## Detailed Behavior

### Connection Establishment
1. When either `subscribe` or `send` is called with a ConnectionRequest:
   - If `action` is Connect:
     a. Initiate a WebSocket connection to the specified `url`.
     b. Handle any connection errors and report them appropriately.
   - If `action` is Disconnect:
     a. If a connection exists for the given `url`, initiate a graceful shutdown.
     b. If no connection exists, log a warning and return.

### On Successful Connection
1. For both `subscribe` and `send`:
   - If `on_connect_payload` is provided:
     a. Immediately send this payload as a one-time message.
     b. Handle any sending errors and report them.

2. Specific to `send`:
   - If `persistent` is true:
     a. Keep the connection open for further send operations.
     b. Implement a mechanism to send data continuously or as needed.
   - If `persistent` is false:
     a. After sending the `on_connect_payload` (if any), initiate a graceful connection closure.

3. Specific to `subscribe`:
   - Persistent means we close the connection after the first message.
   - Set up appropriate event handlers to receive incoming messages.

### Disconnection Behavior
1. When a Disconnect action is received:
   - For `send`:
     a. Stop any ongoing send operations.
     b. Close the connection gracefully.
   - For `subscribe`:
     a. Stop processing incoming messages.
     b. Close the connection gracefully.

2. Ensure all resources are properly cleaned up on disconnection.

### Error Handling
1. Implement robust error handling for all operations:
   - Connection failures
   - Send/Receive errors
   - Disconnection errors

2. Provide meaningful error messages and appropriate error codes.

3. Implement a logging mechanism for debugging and monitoring.

### Thread Safety
1. Ensure that the interface is thread-safe, allowing calls from multiple threads.

2. Implement appropriate synchronization mechanisms to handle concurrent requests.

### Performance Considerations
1. Optimize for high-throughput scenarios, especially for persistent connections.

2. Implement efficient buffer management for send/receive operations.

3. Consider implementing connection pooling for frequently accessed URLs.

### Extensibility
1. Design the system to be easily extensible for future ActionTypes (e.g., Ping).

2. Allow for easy addition of new features to the ConnectionRequest structure.

## Implementation Notes
- Use Boost.Beast for WebSocket implementation.
- Implement asynchronous operations for better performance and responsiveness.
- Consider using a state machine design for managing connection lifecycles.
- Implement a robust reconnection strategy for persistent connections.
```
"""
