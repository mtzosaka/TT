# Distributed Timestamp System

A robust system for synchronized timestamp acquisition across distributed Time Controllers.

## Overview

The Distributed Timestamp System enables precise timestamp synchronization between multiple Time Controllers in a distributed setup. It consists of a master controller and one or more slave agents that work together to collect and synchronize timestamps with nanosecond precision.

## Features

- **Precise Synchronization**: Synchronizes timestamps between distributed Time Controllers with nanosecond precision
- **Robust Communication**: Implements reliable ZeroMQ-based communication with retry mechanisms and error handling
- **Flexible Configuration**: Supports customizable acquisition parameters and port assignments
- **Comprehensive Diagnostics**: Provides detailed logging and status reporting for troubleshooting
- **Data Analysis**: Generates both binary and human-readable output formats for timestamp data
- **Synchronization Reports**: Creates detailed reports on synchronization quality and offset statistics

## System Architecture

The system follows a master-slave architecture:

- **Master Controller**: Coordinates the acquisition process, triggers synchronized data collection, and processes the master Time Controller data
- **Slave Agent**: Responds to commands from the master, collects data from the slave Time Controller, and sends it back to the master

Communication between components uses ZeroMQ sockets:
- Trigger Socket: For sending trigger commands from master to slave
- Command Socket: For sending control commands and receiving responses
- File Socket: For transferring data files from slave to master
- Sync Socket: For synchronization handshake between master and slave

## Requirements

- Linux-based operating system
- ZeroMQ library (libzmq3-dev)
- C++17 compatible compiler
- CMake 3.10 or higher
- ID Quantique Time Controllers accessible via network

## Building the System

1. Extract the package to a directory of your choice
2. Navigate to the extracted directory
3. Run the build script:

```bash
./build.sh
```

This will create the `master_timestamp` and `slave_timestamp` executables in the `build` directory.

### Quick Start Example

To quickly verify that the master and slave communicate correctly, open two terminal windows and run the components with their default port numbers. Start the slave first:

```bash
./build/slave_timestamp --slave-tc 127.0.0.1 --master-address 127.0.0.1 --verbose
```

Then launch the master:

```bash
./build/master_timestamp --master-tc 127.0.0.1 --slave-address 127.0.0.1 \
  --duration 2.0 --channels 1,2,3,4 --verbose
```

Both programs will use the default port values listed below, so no extra options are required unless you need to change the ports.

## Usage

### Running the Slave

Start the slave component first. Make sure the port values match the master configuration:

```bash
./slave_timestamp \
  --slave-tc <IP> \
  --master-address <IP> \
  --trigger-port 5557 \
  --status-port 5559 \
  --file-port 5560 \
  --command-port 5561 \
  --sync-port 5562 \
  --verbose --text-output
```

### Running the Master

After the slave is running, start the master component with the same port numbers:

```bash
./master_timestamp \
  --master-tc <IP> \
  --slave-address <IP> \
  --trigger-port 5557 \
  --status-port 5559 \
  --file-port 5560 \
  --command-port 5561 \
  --sync-port 5562 \
  --duration <seconds> \
  --channels <list> \
  --verbose --text-output
```

### Command-Line Options

#### Master Options

- `--master-tc ADDRESS`: Address of local Time Controller (default: 127.0.0.1)
- `--slave-address ADDRESS` or `--slave ADDRESS`: Address of slave PC (default: 127.0.0.1)
- `--trigger-port PORT`: Port for trigger messages (default: 5557)
- `--status-port PORT`: Port for status updates (default: 5559)
- `--file-port PORT`: Port for file transfer (default: 5560)
- `--command-port PORT`: Port for command messages (default: 5561)
- `--sync-port PORT`: Port for synchronization handshake (default: 5562)
- `--output-dir DIR`: Directory for output files (default: ./outputs)
- `--duration SECONDS`: Acquisition duration in seconds (default: 0.6)
- `--channels LIST`: Comma-separated list of channels (default: 1,2,3,4)
- `--verbose`: Enable verbose output
- `--text-output`: Generate human-readable text output files
- `--help`: Display help message

#### Slave Options

- `--slave-tc ADDRESS`: Address of local Time Controller (default: 127.0.0.1)
- `--master-address ADDRESS`: Address of master PC (default: 127.0.0.1)
- `--trigger-port PORT`: Port for trigger messages (default: 5557)
- `--status-port PORT`: Port for status updates (default: 5559)
- `--file-port PORT`: Port for file transfer (default: 5560)
- `--command-port PORT`: Port for command messages (default: 5561)
- `--sync-port PORT`: Port for synchronization handshake (default: 5562)
- `--output-dir DIR`: Directory for output files (default: ./outputs)
- `--verbose`: Enable verbose output
- `--text-output`: Generate human-readable text output files
- `--help`: Display help message

## Output Files

The system generates several output files:

- `master_results_YYYYMMDD_HHMMSS.bin`: Binary file with master timestamps
- `master_results_YYYYMMDD_HHMMSS.txt`: Text file with master timestamps (if --text-output is used)
- `master_results_YYYYMMDD_HHMMSS_corrected.bin`: Binary file with corrected master timestamps
- `master_results_YYYYMMDD_HHMMSS_corrected.txt`: Text file with corrected timestamps (if --text-output is used)
- `master_results_YYYYMMDD_HHMMSS_offset_report.txt`: Report on synchronization quality
- `slave_results_YYYYMMDD_HHMMSS.bin`: Binary file with slave timestamps
- `slave_results_YYYYMMDD_HHMMSS.txt`: Text file with slave timestamps (if --text-output is used)

## Troubleshooting

### Common Issues

1. **Connection Failures**:
   - Ensure both master and slave can ping each other
   - Check that all required ports are open in firewalls
   - Verify that the IP addresses are correct

2. **Synchronization Timeouts**:
   - Ensure both master and slave are using the same sync port
   - Check network latency between master and slave
   - Increase timeout values if necessary

3. **Data Collection Issues**:
   - Verify that Time Controllers are properly connected and responding
   - Check that the specified channels exist on the Time Controllers
   - Ensure sufficient acquisition duration for meaningful data collection

### Diagnostic Steps

1. Run both components with `--verbose` flag for detailed logging
2. Check that all sockets are binding/connecting to the correct addresses and ports
3. Verify that the handshake protocol completes successfully
4. Examine the synchronization offset report for quality assessment

## Advanced Configuration

For complex network setups or specific requirements, you may need to adjust the following:

- Socket timeout values in the source code
- Retry counts for synchronization attempts
- Buffer sizes for data transfer

## License

This software is proprietary and confidential.

## Contact

For support or questions, please contact the development team.
