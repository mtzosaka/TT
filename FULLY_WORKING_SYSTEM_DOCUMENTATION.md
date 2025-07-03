# Distributed Timestamp System - Fully Working Version

## Summary

This is the complete, fully working distributed timestamp system with all major issues resolved:

1. ✅ **Crash Issue Fixed**: No more "terminate called without an active exception" crashes
2. ✅ **File Transfer Fixed**: Binary files transfer successfully using base64 encoding
3. ✅ **Real Data Collection**: Collects actual timestamp data from Time Controller inputs
4. ✅ **Proper Synchronization**: Master-slave synchronization works correctly

## Key Fixes Implemented

### 1. Crash Prevention
- **Root Cause**: Complex data processing and thread management issues
- **Solution**: Integrated working data collection code from DataLinkTargetService
- **Result**: System completes acquisitions without crashes

### 2. File Transfer Fix
- **Root Cause**: Binary data in JSON causing UTF-8 encoding errors
- **Solution**: Implemented base64 encoding/decoding for binary file transfer
- **Result**: Binary files transfer successfully from slave to master

### 3. Real Data Collection
- **Root Cause**: System was generating dummy data instead of collecting real timestamps
- **Solution**: Implemented proper Time Controller acquisition sequence:
  ```cpp
  // Channel configuration
  zmq_exec(tc, "RAW" + ch + ":REF:LINK NONE");
  zmq_exec(tc, "RAW" + ch + ":ERRORS:CLEAR");
  zmq_exec(tc, "RAW" + ch + ":SEND ON");
  
  // Acquisition sequence
  zmq_exec(tc, "REC:PLAY");    // Start acquisition
  sleep(duration);             // Wait for specified time
  zmq_exec(tc, "REC:STOP");    // Stop acquisition
  
  // Data retrieval
  count = zmq_exec(tc, "RAW" + ch + ":DATA:COUNt?");
  for (i = 0; i < count; i++) {
      timestamp = zmq_exec(tc, "RAW" + ch + ":DATA:VALue? " + i);
  }
  ```
- **Result**: Collects real timestamp data from Time Controller inputs

### 4. Enhanced Error Handling
- **Global terminate handler**: Catches uncaught exceptions gracefully
- **Comprehensive try-catch blocks**: Protects all critical operations
- **Graceful degradation**: System continues even if individual operations fail

## System Architecture

```
Master Controller (192.168.0.1)          Slave Agent (192.168.0.2)
├── Time Controller: 169.254.216.107     ├── Time Controller: 169.254.218.109
├── Trigger Socket (PUB): 5557           ├── Trigger Socket (SUB): 5557
├── File Socket (PULL): 5559             ├── File Socket (PUSH): 5559
├── Command Socket (REQ): 5560           ├── Command Socket (REP): 5560
└── Sync Socket (PULL): 5562             └── Sync Socket (PUSH): 5562
```

## Usage Instructions

### 1. Start Slave Agent (First)
```bash
./slave_timestamp --slave-tc 169.254.218.109 --master-address 192.168.0.1 --sync-port 5562 --verbose --text-output
```

### 2. Start Master Controller (Second)
```bash
./master_timestamp --master-tc 169.254.216.107 --slave-address 192.168.0.2 --sync-port 5562 --duration 1 --channels 1,2,3,4 --verbose --text-output
```

## Expected Behavior

1. **Initialization**: Both components connect to their Time Controllers and establish communication
2. **Synchronization**: Master requests ready signal, slave responds when ready
3. **Acquisition**: Both start synchronized timestamp collection for specified duration
4. **Data Processing**: Real timestamp data is collected from Time Controller inputs
5. **File Transfer**: Slave sends binary files to master using base64 encoding
6. **Completion**: System exits cleanly without crashes

## Output Files

### Master Output
- `master_results_YYYYMMDD_HHMMSS.bin` - Binary timestamp data
- `master_results_YYYYMMDD_HHMMSS.txt` - Text timestamp data
- `master_results_YYYYMMDD_HHMMSS_corrected.bin` - Offset-corrected binary data
- `master_results_YYYYMMDD_HHMMSS_corrected.txt` - Offset-corrected text data
- `master_results_YYYYMMDD_HHMMSS_offset_report.txt` - Synchronization analysis

### Slave Output
- `slave_results_YYYYMMDD_HHMMSS.bin` - Binary timestamp data
- `slave_results_YYYYMMDD_HHMMSS.txt` - Text timestamp data

## Technical Details

### Data Collection Method
- Uses proven zmq_exec() commands from working DataLinkTargetService
- Proper acquisition timing with REC:PLAY → wait → REC:STOP sequence
- Real data retrieval using RAW:DATA:COUNt? and RAW:DATA:VALue? commands
- Only collects data from channels with actual input signals

### File Transfer Protocol
- JSON-based messaging with base64-encoded binary data
- Reliable ZeroMQ PUSH/PULL sockets for file transfer
- Automatic base64 encoding on slave side, decoding on master side
- Error handling for transfer failures

### Synchronization Protocol
- Handshake-based synchronization using dedicated sync sockets
- Master requests ready signal, slave responds when prepared
- Synchronized trigger commands with timestamp information
- Offset calculation and correction for time synchronization

## Troubleshooting

### Common Issues
1. **Connection Errors**: Check IP addresses and network connectivity
2. **Time Controller Errors**: Verify Time Controller addresses and availability
3. **Permission Errors**: Ensure output directories are writable
4. **Port Conflicts**: Check that required ports (5557, 5559, 5560, 5562) are available

### Debug Options
- Use `--verbose` flag for detailed logging
- Use `--text-output` flag to generate human-readable timestamp files
- Check output directory for generated files and error logs

## System Requirements

- Linux environment (tested on Ubuntu)
- ZeroMQ library
- C++17 compatible compiler
- Network connectivity between master and slave
- Access to ID Quantique Time Controllers

This version represents the complete, production-ready distributed timestamp system with all major issues resolved and full functionality implemented.

