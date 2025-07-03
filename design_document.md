# Enhanced Distributed Timestamp System Design

## Overview

This document outlines the design for enhancing the distributed timestamp system to meet the following requirements:

1. Establish master-slave communication and record trigger timestamps for synchronization
2. Collect data locally from Time Controllers and store as .bin files
3. Exchange the first 10% of data for synchronization calculations
4. Support both single file output and streaming multiple files (up to 10 per run)

## Current System Analysis

### Distributed System
- Uses socket-based communication between master and slave
- Performs basic clock synchronization via handshake
- Collects timestamps from Time Controllers
- Transfers complete data from slave to master
- Merges and saves data as binary files

### Timestamp Client System
- Connects to a single Time Controller
- Uses ZeroMQ for communication
- Supports streaming acquisition with sub-acquisitions
- Merges timestamps from multiple channels
- Outputs data as text files

## Design Modifications

### 1. Master-Slave Communication and Trigger Timestamp Recording

#### Master Component:
- Enhance the handshake protocol to include precise trigger timestamps
- Record the exact time when acquisition starts on both master and slave
- Store these trigger timestamps for later synchronization calculations

```cpp
// Master side trigger timestamp recording
uint64_t master_trigger_time = now_ns();
// Send trigger command with timestamp
json trigger_cmd;
trigger_cmd["type"] = "trigger";
trigger_cmd["timestamp"] = master_trigger_time;
trigger_cmd["duration"] = duration;
trigger_cmd["channels"] = channels;
```

#### Slave Component:
- Record local timestamp when trigger is received
- Store both the master trigger time and local trigger time
- Use these for initial synchronization calculations

```cpp
// Slave side trigger timestamp recording
uint64_t slave_trigger_time = now_ns();
uint64_t master_trigger_time = trigger["timestamp"].get<uint64_t>();
// Store both timestamps for sync calculation
```

### 2. Local Data Collection and Binary File Storage

#### Both Components:
- Modify acquisition code to store data in binary format
- Implement a flexible file naming scheme
- Support both single file and multi-file modes

```cpp
// Binary file storage
std::string filename = generate_filename(file_index, streaming_mode);
std::ofstream file(filename, std::ios::binary);
for (const auto& timestamp : timestamps) {
    file.write(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));
}
```

### 3. Partial Data Exchange (10%)

#### Slave Component:
- Implement a function to extract the first 10% of data
- Send only this portion to the master for synchronization

```cpp
// Extract and send first 10% of data
size_t partial_size = timestamps.size() / 10;
std::vector<uint64_t> partial_data(timestamps.begin(), 
                                  timestamps.begin() + partial_size);
send_partial_data(partial_data);
```

#### Master Component:
- Receive partial data from slave
- Use it for synchronization calculations
- Store the result for later reference

```cpp
// Receive and process partial data
std::vector<uint64_t> slave_partial_data = receive_partial_data();
calculate_synchronization(master_data, slave_partial_data, 
                         master_trigger_time, slave_trigger_time);
```

### 4. Flexible File Output

#### Configuration:
- Add parameters for file mode (single/streaming)
- Add parameters for number of files (1-10)
- Add parameters for sub-duration

```cpp
struct AcquisitionConfig {
    bool streaming_mode;
    int num_files;
    double file_duration;
    double sub_duration;
};
```

#### Implementation:
- For single file mode: collect all data into one file
- For streaming mode: create multiple files with sequential naming
- Ensure first file contains at least 10% of total data for sync

```cpp
// File management logic
if (config.streaming_mode) {
    for (int i = 0; i < config.num_files; i++) {
        // Create file i
        // Collect data for file_duration
        // Close file
    }
} else {
    // Create single file
    // Collect data for total_duration
    // Close file
}
```

### 5. Synchronization Calculation

#### Algorithm:
- Use trigger timestamps as initial reference points
- Calculate offset between master and slave clocks
- Apply corrections to slave timestamps
- Write synchronization results to a separate file

```cpp
// Sync calculation
int64_t trigger_offset = slave_trigger_time - master_trigger_time;
// Fine-tune using partial data comparison
int64_t fine_offset = calculate_fine_offset(master_partial_data, 
                                          slave_partial_data);
// Apply correction
for (auto& ts : slave_timestamps) {
    ts = ts - (trigger_offset + fine_offset);
}
// Write sync info to file
write_sync_info(trigger_offset, fine_offset);
```

## Integration Plan

1. Modify the master controller to:
   - Support flexible file modes
   - Record trigger timestamps
   - Process partial data for sync
   - Calculate and store synchronization information

2. Modify the slave agent to:
   - Record trigger timestamps
   - Store data locally in binary format
   - Send partial data (10%) to master
   - Support flexible file modes

3. Integrate timestamp client features:
   - Sub-acquisition support
   - ZeroMQ communication
   - Channel configuration

4. Create a unified configuration system:
   - JSON-based configuration
   - Command-line parameter support
   - Default values for all parameters

## Implementation Priorities

1. Trigger timestamp recording and synchronization
2. Binary file storage
3. Partial data exchange
4. Flexible file output modes
5. Integration and testing

This design provides a comprehensive approach to meeting all the requirements while maintaining compatibility with the existing system architecture.
