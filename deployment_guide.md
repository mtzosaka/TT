# Distributed Timestamp System - Deployment and Usage Guide

## Overview

This guide provides comprehensive instructions for deploying and using the distributed timestamp system across two Ubuntu PCs, each connected to a Time Controller device. The system enables picosecond-level synchronized timestamp collection with real-time file transfer capabilities.

## System Requirements

- **Operating System**: Ubuntu 22.04 or compatible Linux distribution
- **Libraries**: ZeroMQ 4.3.4 or newer
- **Network**: Gigabit Ethernet recommended, with minimal latency between PCs
- **Hardware**: Two Time Controller devices, one connected to each PC
- **Permissions**: Administrative (sudo) privileges for installation

## Installation

### 1. Install Dependencies

On both Master and Slave PCs:

```bash
# Update package lists
sudo apt-get update

# Install build tools and ZeroMQ
sudo apt-get install -y build-essential cmake pkg-config
sudo apt-get install -y libzmq3-dev
```

### 2. Build the System

On both Master and Slave PCs:

```bash
# Clone or extract the source code
mkdir -p ~/distributed_timestamp
cd ~/distributed_timestamp

# Copy source files to this directory
# (master_controller.cpp, master_controller.hpp, slave_agent.cpp, slave_agent.hpp, etc.)

# Create build directory
mkdir -p build
cd build

# Configure and build
cmake ..
make

# Optionally install system-wide
sudo make install
```

## Network Configuration

### 1. Configure Network Interfaces

Ensure both PCs have static IP addresses and can communicate with each other:

```bash
# Check connectivity from Master to Slave
ping <slave_ip_address>

# Check connectivity from Slave to Master
ping <master_ip_address>
```

### 2. Optimize Network for Real-time Performance

For optimal real-time performance, consider these network optimizations:

```bash
# Disable network power saving on both PCs
sudo ethtool -s eth0 wol d

# Set network interface to maximum performance
sudo ethtool -s eth0 speed 1000 duplex full autoneg off

# Increase network buffer sizes
sudo sysctl -w net.core.rmem_max=26214400
sudo sysctl -w net.core.wmem_max=26214400
sudo sysctl -w net.core.rmem_default=26214400
sudo sysctl -w net.core.wmem_default=26214400
```

### 3. Configure Firewall (if active)

Ensure the following ports are open for communication:

- 5557: Trigger messages
- 5559: Status updates
- 5560: File transfer
- 5561: Command messages

```bash
# Example using ufw
sudo ufw allow 5557/tcp
sudo ufw allow 5559/tcp
sudo ufw allow 5560/tcp
sudo ufw allow 5561/tcp
```

## Time Controller Setup

### 1. Verify Time Controller Connectivity

On both Master and Slave PCs:

```bash
# Test connection to Time Controller
# Replace with actual command for your Time Controller model
tc_connect --address <time_controller_ip>
```

### 2. Synchronize Time Controllers

For picosecond-level synchronization, ensure both Time Controllers are properly synchronized:

1. Connect both Time Controllers to a common reference clock if available
2. Configure Time Controllers to use the same reference source
3. Verify synchronization using the Time Controller's built-in tools

## Running the System

### 1. Start the Slave Agent

On the Slave PC:

```bash
# Basic usage
slave_timestamp --master <master_ip_address>

# Full options
slave_timestamp \
  --local-tc <time_controller_ip> \
  --master <master_ip_address> \
  --trigger-port 5557 \
  --status-port 5559 \
  --file-port 5560 \
  --command-port 5561 \
  --output-dir /path/to/output/directory
```

The slave agent will start and wait for trigger commands from the master.

### 2. Run the Master Controller

On the Master PC:

```bash
# Basic usage
master_timestamp --slave <slave_ip_address> --duration 0.6

# Full options
master_timestamp \
  --local-tc <time_controller_ip> \
  --slave <slave_ip_address> \
  --trigger-port 5557 \
  --status-port 5559 \
  --file-port 5560 \
  --command-port 5561 \
  --output-dir /path/to/output/directory \
  --duration 0.6 \
  --channels "1,2,3,4"
```

The master controller will:
1. Trigger synchronized acquisition on both local and remote Time Controllers
2. Monitor status updates from the slave
3. Receive and store timestamp files from the slave
4. Exit when the acquisition and file transfer are complete

## Command-Line Options

### Master Controller Options

| Option | Description | Default |
|--------|-------------|---------|
| `--local-tc ADDRESS` | Address of local Time Controller | 127.0.0.1 |
| `--slave ADDRESS` | Address of slave PC | 127.0.0.1 |
| `--trigger-port PORT` | Port for trigger messages | 5557 |
| `--status-port PORT` | Port for status updates | 5559 |
| `--file-port PORT` | Port for file transfer | 5560 |
| `--command-port PORT` | Port for command messages | 5561 |
| `--output-dir DIR` | Directory for output files | ./outputs |
| `--duration SECONDS` | Acquisition duration in seconds | 0.6 |
| `--channels LIST` | Comma-separated list of channels | 1,2,3,4 |
| `--help` | Display help message | - |

### Slave Agent Options

| Option | Description | Default |
|--------|-------------|---------|
| `--local-tc ADDRESS` | Address of local Time Controller | 127.0.0.1 |
| `--master ADDRESS` | Address of master PC | 127.0.0.1 |
| `--trigger-port PORT` | Port for trigger messages | 5557 |
| `--status-port PORT` | Port for status updates | 5559 |
| `--file-port PORT` | Port for file transfer | 5560 |
| `--command-port PORT` | Port for command messages | 5561 |
| `--output-dir DIR` | Directory for output files | ./outputs |
| `--help` | Display help message | - |

## Output Files

After a successful acquisition, the following files will be created:

1. **Master PC**: 
   - Local timestamp file in the specified output directory
   - Transferred timestamp file from the slave with prefix "slave_"

2. **Slave PC**:
   - Local timestamp file in the specified output directory

## Monitoring and Troubleshooting

### Monitoring Status

The master controller displays real-time status updates from the slave during acquisition:

```
Slave status: running (45.2%)
Slave status: running (89.7%)
Slave status: completed (100%)
```

### Common Issues and Solutions

#### Connection Problems

**Issue**: Master cannot connect to slave
**Solution**: 
- Verify network connectivity with ping
- Check that slave agent is running
- Ensure firewall allows required ports

#### Synchronization Issues

**Issue**: Poor timestamp synchronization
**Solution**:
- Verify Time Controllers are properly synchronized
- Check network latency between PCs
- Consider using a dedicated network for timestamp traffic

#### File Transfer Problems

**Issue**: File transfer fails or is incomplete
**Solution**:
- Check available disk space
- Verify network stability
- Increase timeout values if necessary

## Performance Tuning

### Real-time Priority

For best real-time performance, run the applications with elevated priority:

```bash
# On both Master and Slave PCs
sudo nice -n -20 master_timestamp [options]
sudo nice -n -20 slave_timestamp [options]
```

### Memory Optimization

Pre-allocate memory to avoid runtime allocations:

```bash
# Disable memory overcommit
sudo sysctl -w vm.overcommit_memory=2

# Disable swapping for the process
sudo chrt -f 99 master_timestamp [options]
sudo chrt -f 99 slave_timestamp [options]
```

### CPU Isolation

For critical real-time applications, consider isolating CPU cores:

```bash
# In /etc/default/grub, add to GRUB_CMDLINE_LINUX:
# isolcpus=2,3

# Then update grub and reboot
sudo update-grub
sudo reboot

# Run the application on isolated cores
taskset -c 2 master_timestamp [options]
```

## Advanced Configuration

### Custom ZeroMQ Settings

For advanced users, ZeroMQ settings can be tuned by modifying the source code:

- Increase high water mark for higher throughput
- Adjust linger period for faster shutdown
- Modify reconnection intervals for better resilience

### Logging Configuration

To enable detailed logging, set the following environment variables:

```bash
# Enable debug logging
export TIMESTAMP_LOG_LEVEL=DEBUG

# Specify log file
export TIMESTAMP_LOG_FILE=/path/to/logfile.log
```

## Best Practices

1. **Network Isolation**: Use a dedicated network for timestamp synchronization
2. **Regular Testing**: Periodically verify synchronization accuracy
3. **Monitoring**: Implement system monitoring for long-term stability
4. **Backup**: Regularly backup configuration and output files
5. **Updates**: Keep all software components updated

## Support and Troubleshooting

For additional support or troubleshooting:

1. Check the test report for performance expectations
2. Review system logs for error messages
3. Contact technical support with detailed information about your setup

## Conclusion

This distributed timestamp system provides picosecond-level synchronization across multiple PCs and Time Controllers. By following this guide, you can deploy, configure, and operate the system for precise timestamp collection and real-time file transfer.
