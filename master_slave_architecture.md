# Master-Slave Architecture for Distributed Timestamp System

## Overview

This document outlines the architecture for a distributed timestamp system across two Ubuntu PCs, each connected to a Time Controller device. The system is designed for picosecond-level synchronization with real-time file transfer capabilities.

## Network Topology

```
PC1 (Master) <---> Switch1 <---> Time Controller 1
                      |
                      | Network Link
                      |
PC2 (Slave)  <---> Switch2 <---> Time Controller 2
```

## System Requirements

1. **Simultaneous Triggering**: Both Time Controllers must be triggered simultaneously for picosecond-level synchronization
2. **Real-time Performance**: Minimize latency in all operations
3. **Automatic File Transfer**: Slave must transfer completed timestamp files to Master
4. **Ubuntu Compatibility**: Both systems run Ubuntu

## Component Architecture

### Master Component (PC1)

1. **Trigger Controller**
   - Sends simultaneous trigger commands to local and remote Time Controllers
   - Uses low-latency ZeroMQ PUB/SUB pattern for broadcasting trigger signals
   - Implements precise timing mechanisms for synchronization

2. **Status Monitor**
   - Tracks the status of both local and remote timestamp collection processes
   - Receives real-time updates from the slave component
   - Implements timeout and error handling for robustness

3. **File Receiver**
   - Accepts incoming timestamp files from the slave component
   - Uses high-performance file transfer protocol (ZeroMQ for binary data)
   - Organizes and stores files in the specified output directory

### Slave Component (PC2)

1. **Command Listener**
   - Listens for trigger commands from the master
   - Implements low-latency response to trigger signals
   - Starts local timestamp collection immediately upon trigger

2. **Status Reporter**
   - Sends real-time status updates to the master
   - Reports collection start, progress, and completion events
   - Includes error reporting for fault tolerance

3. **File Sender**
   - Transfers completed timestamp files to the master
   - Uses the same high-performance protocol as the File Receiver
   - Implements verification to ensure data integrity

## Communication Protocols

### Trigger Protocol (ZeroMQ PUB/SUB)
- Master publishes trigger commands on a dedicated channel
- Slave subscribes to the trigger channel
- Minimal message format for reduced latency
- Includes timestamp for synchronization verification

### Status Protocol (ZeroMQ REQ/REP)
- Slave sends status updates to master
- Master acknowledges receipt
- Includes progress information and error codes
- Periodic heartbeat for connection verification

### File Transfer Protocol (ZeroMQ PUSH/PULL)
- High-throughput binary data transfer
- Chunked transfer for large files
- CRC verification for data integrity
- Progress tracking and resumable transfers

## Synchronization Strategy

1. **Network Time Synchronization**
   - Both PCs synchronized using PTP (Precision Time Protocol) or similar
   - Sub-microsecond clock synchronization between systems

2. **Trigger Synchronization**
   - Broadcast trigger with minimal network hops
   - Compensate for known network latencies
   - Use hardware timestamps when available

3. **Verification**
   - Compare timestamps from both systems for drift analysis
   - Implement adaptive correction if necessary
   - Log synchronization metrics for analysis

## Performance Considerations

1. **Minimizing Latency**
   - Direct network connections when possible
   - Kernel bypass techniques for network operations
   - Real-time priority for critical processes
   - Minimal data serialization overhead

2. **Optimizing Throughput**
   - Efficient file transfer with minimal copying
   - Parallel transfer streams for large files
   - Compression only when beneficial for transfer time

3. **Resource Management**
   - Dedicated CPU cores for critical operations
   - Memory pre-allocation to avoid runtime allocations
   - Disk I/O optimization for timestamp file writing

## Fault Tolerance

1. **Connection Monitoring**
   - Heartbeat mechanism between master and slave
   - Automatic reconnection after network interruptions
   - Timeout handling for all operations

2. **Error Recovery**
   - Graceful degradation on component failure
   - Local operation capability when network is unavailable
   - Resumable file transfers after interruption

3. **Data Integrity**
   - Verification of all transferred files
   - Checksums for critical command messages
   - Transaction logging for recovery purposes

## Implementation Approach

The implementation will extend the existing timestamp client application with:

1. Command-line flags to specify master or slave mode
2. Configuration options for network addresses and ports
3. New modules for the distributed functionality
4. Minimal changes to the core timestamp collection logic

This approach ensures backward compatibility while adding the new distributed capabilities.
