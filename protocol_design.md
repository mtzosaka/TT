# Protocol Design for Distributed Timestamp System

## 1. Trigger Protocol

### Purpose
Enable simultaneous triggering of timestamp collection on both master and slave systems with minimal latency.

### Protocol Details
- **Pattern**: ZeroMQ PUB/SUB
- **Transport**: TCP for reliability, UDP for lowest latency (configurable)
- **Addressing**: Master publishes to `tcp://*:5557` or `udp://*:5557`
- **Subscription**: Slave subscribes to `tcp://master_ip:5557` or `udp://master_ip:5557`

### Message Format
```json
{
  "type": "trigger",
  "timestamp": 1621234567890123,
  "duration": 0.6,
  "channels": [1, 2, 3, 4],
  "sequence": 42
}
```

- `type`: Always "trigger" for trigger messages
- `timestamp`: Nanosecond-precision timestamp when trigger was issued (UTC)
- `duration`: Acquisition duration in seconds
- `channels`: Array of channels to record
- `sequence`: Monotonically increasing sequence number for deduplication

### Timing Considerations
- Master sends trigger message simultaneously to local and remote timestamp processes
- Slave processes trigger immediately upon receipt
- Both systems record actual trigger execution time for later analysis
- Network time synchronization (PTP) should be established before operation

## 2. Status Protocol

### Purpose
Provide real-time status updates from slave to master about timestamp collection progress.

### Protocol Details
- **Pattern**: ZeroMQ REQ/REP for status queries, PUSH/PULL for asynchronous updates
- **Transport**: TCP
- **Addressing**: 
  - Status query: Slave binds REP socket to `tcp://*:5558`
  - Status updates: Slave connects PUSH socket to `tcp://master_ip:5559`

### Message Format
```json
{
  "type": "status",
  "state": "running",
  "progress": 75.5,
  "acquisitions_count": 42,
  "timestamp": 1621234567890123,
  "sequence": 43,
  "error": null
}
```

- `type`: Always "status" for status messages
- `state`: One of "idle", "starting", "running", "completed", "error"
- `progress`: Percentage complete (0-100)
- `acquisitions_count`: Number of sub-acquisitions completed
- `timestamp`: Nanosecond-precision timestamp of status update
- `sequence`: Monotonically increasing sequence number
- `error`: Error description if state is "error", otherwise null

### Heartbeat Mechanism
- Slave sends heartbeat status update every 100ms during acquisition
- Master considers slave disconnected if no heartbeat received for 500ms
- Heartbeat messages are minimal format with only required fields

## 3. File Transfer Protocol

### Purpose
Transfer completed timestamp files from slave to master with maximum throughput and reliability.

### Protocol Details
- **Pattern**: ZeroMQ PUSH/PULL for data transfer
- **Transport**: TCP
- **Addressing**: Slave connects PUSH socket to `tcp://master_ip:5560`

### Message Format
```json
{
  "type": "file_header",
  "filename": "results_20250527_123456.txt",
  "size": 1048576,
  "chunks": 16,
  "checksum": "a1b2c3d4e5f6...",
  "sequence": 44
}
```

```
[Binary chunk data]
```

```json
{
  "type": "file_footer",
  "filename": "results_20250527_123456.txt",
  "chunks_sent": 16,
  "checksum": "a1b2c3d4e5f6...",
  "sequence": 60
}
```

### Transfer Process
1. Slave sends file_header message with metadata
2. Slave sends binary chunks (64KB each) sequentially
3. Slave sends file_footer message to confirm completion
4. Master assembles chunks and verifies checksum
5. Master sends acknowledgment message

### Optimization Techniques
- Pre-allocated buffers for chunk transfer
- Zero-copy when supported by platform
- Multiple concurrent transfers for different files
- Adaptive chunk sizing based on network conditions

## 4. Command Protocol

### Purpose
Allow master to send commands to slave for operations beyond triggering.

### Protocol Details
- **Pattern**: ZeroMQ REQ/REP
- **Transport**: TCP
- **Addressing**: Slave binds REP socket to `tcp://*:5561`

### Message Format
```json
{
  "type": "command",
  "command": "stop",
  "params": {},
  "sequence": 45
}
```

- `type`: Always "command" for command messages
- `command`: One of "stop", "status", "reset", "configure"
- `params`: Command-specific parameters
- `sequence`: Monotonically increasing sequence number

### Response Format
```json
{
  "type": "response",
  "command": "stop",
  "success": true,
  "data": {},
  "error": null,
  "sequence": 45
}
```

## 5. Discovery Protocol

### Purpose
Enable automatic discovery of master and slave systems on the network.

### Protocol Details
- **Pattern**: ZeroMQ PUB/SUB with multicast
- **Transport**: UDP
- **Addressing**: All nodes publish to `udp://239.192.1.1:5562`

### Message Format
```json
{
  "type": "discovery",
  "node_type": "master",
  "node_id": "master-pc1",
  "ip": "192.168.1.100",
  "services": {
    "trigger": 5557,
    "status": 5559,
    "file": 5560,
    "command": 5561
  },
  "timestamp": 1621234567890123
}
```

## Implementation Considerations

### Latency Minimization
- Use of real-time priority for network threads
- Kernel bypass techniques where available
- Minimal serialization overhead
- Pre-established connections

### Reliability
- Sequence numbers for all messages
- Checksums for data integrity
- Acknowledgments for critical operations
- Automatic reconnection

### Security
- Optional TLS encryption for all connections
- Authentication for command messages
- Verification of node identities

### Configuration
- All ports and addresses configurable
- Protocol selection (TCP/UDP) configurable
- Performance parameters tunable
