# Complete Deployment Guide: Distributed Timestamp System

## Introduction

This comprehensive guide provides detailed instructions for deploying the distributed timestamp system across two Ubuntu machines. The system enables picosecond-level synchronized timestamp collection with real-time file transfer capabilities.

## Table of Contents

1. [System Overview](#system-overview)
2. [Required Files](#required-files)
3. [Hardware Requirements](#hardware-requirements)
4. [Network Setup](#network-setup)
5. [Software Installation](#software-installation)
6. [Configuration](#configuration)
7. [Running the System](#running-the-system)
8. [Verification and Testing](#verification-and-testing)
9. [Troubleshooting](#troubleshooting)
10. [Performance Tuning](#performance-tuning)
11. [Maintenance](#maintenance)

## System Overview

The distributed timestamp system consists of two main components:

1. **Master Controller (PC1)**: Initiates synchronized timestamp collection, monitors status, and receives files from the slave.
2. **Slave Agent (PC2)**: Responds to trigger commands, collects timestamps, and transfers files to the master.

Both components communicate via ZeroMQ for low-latency, reliable messaging and file transfer.

## Required Files

The following files are included in the deployment package:

### Source Code Files
- `master_controller.cpp` - Implementation of the master controller
- `master_controller.hpp` - Header file for the master controller
- `slave_agent.cpp` - Implementation of the slave agent
- `slave_agent.hpp` - Header file for the slave agent
- `master_main.cpp` - Main entry point for the master application
- `slave_main.cpp` - Main entry point for the slave application
- `mock_time_controller.cpp` - Test utility for simulating a Time Controller
- `network_test.cpp` - Utility for testing network performance
- `CMakeLists.txt` - Build configuration file

### Documentation Files
- `master_slave_architecture.md` - Detailed architecture description
- `protocol_design.md` - Communication protocol specifications
- `deployment_guide.md` - This deployment guide
- `dual_machine_deployment_guide.md` - Specific guide for physical machine setup
- `test_report.md` - Performance test results and analysis

### Configuration Files
- `master_config.json` - Example configuration for master controller
- `slave_config.json` - Example configuration for slave agent

### Test and Utility Files
- `timestamp_analysis.cpp` - Utility for analyzing timestamp data
- `network_benchmark.sh` - Script for network performance testing

## Hardware Requirements

### Master Machine (PC1)
- Ubuntu 22.04 LTS operating system
- Minimum 4GB RAM, 2 CPU cores
- Ethernet port (Gigabit recommended)
- Connection to Time Controller device

### Slave Machine (PC2)
- Ubuntu 22.04 LTS operating system
- Minimum 4GB RAM, 2 CPU cores
- Ethernet port (Gigabit recommended)
- Connection to Time Controller device

### Network Equipment
- Gigabit Ethernet switch
- Cat6 or better Ethernet cables
- Optional: Dedicated network for timestamp traffic

### Time Controller Requirements
- Two compatible Time Controller devices
- Support for external triggering
- Picosecond-level timestamp precision
- Network or direct connection to respective PCs

## Network Setup

### Physical Connection

1. Connect both PCs to the same Ethernet switch
2. Connect each Time Controller to its respective PC
3. Ensure all connections are secure and using quality cables

### IP Address Configuration

On Master Machine (PC1):
```bash
# Edit network configuration
sudo nano /etc/netplan/01-netcfg.yaml
```

Add the following configuration (adjust interface name as needed):
```yaml
network:
  version: 2
  renderer: networkd
  ethernets:
    ens33:
      dhcp4: no
      addresses: [192.168.1.10/24]
      gateway4: 192.168.1.1
      nameservers:
        addresses: [8.8.8.8, 8.8.4.4]
```

Apply the configuration:
```bash
sudo netplan apply
```

On Slave Machine (PC2):
```bash
# Edit network configuration
sudo nano /etc/netplan/01-netcfg.yaml
```

Add the following configuration (adjust interface name as needed):
```yaml
network:
  version: 2
  renderer: networkd
  ethernets:
    ens33:
      dhcp4: no
      addresses: [192.168.1.11/24]
      gateway4: 192.168.1.1
      nameservers:
        addresses: [8.8.8.8, 8.8.4.4]
```

Apply the configuration:
```bash
sudo netplan apply
```

### Network Optimization

On both machines, optimize network settings for low latency:

```bash
# Disable network power saving
sudo ethtool -s ens33 wol d

# Set network interface to maximum performance
sudo ethtool -s ens33 speed 1000 duplex full autoneg off

# Increase network buffer sizes
sudo sysctl -w net.core.rmem_max=26214400
sudo sysctl -w net.core.wmem_max=26214400
sudo sysctl -w net.core.rmem_default=26214400
sudo sysctl -w net.core.wmem_default=26214400

# Make settings persistent
echo "net.core.rmem_max=26214400" | sudo tee -a /etc/sysctl.conf
echo "net.core.wmem_max=26214400" | sudo tee -a /etc/sysctl.conf
echo "net.core.rmem_default=26214400" | sudo tee -a /etc/sysctl.conf
echo "net.core.wmem_default=26214400" | sudo tee -a /etc/sysctl.conf
```

### Firewall Configuration

On both machines, configure the firewall to allow required ports:

```bash
# Allow ZeroMQ communication ports
sudo ufw allow 5557/tcp
sudo ufw allow 5559/tcp
sudo ufw allow 5560/tcp
sudo ufw allow 5561/tcp

# Enable the firewall if not already enabled
sudo ufw enable
```

### Verify Network Connectivity

On Master Machine (PC1):
```bash
# Ping the Slave Machine
ping 192.168.1.11

# Check network latency
ping -c 100 192.168.1.11 | grep rtt
```

On Slave Machine (PC2):
```bash
# Ping the Master Machine
ping 192.168.1.10

# Check network latency
ping -c 100 192.168.1.10 | grep rtt
```

For optimal performance, the average round-trip time should be less than 1ms.

## Software Installation

### 1. Install Dependencies

On both Master and Slave machines:

```bash
# Update package lists
sudo apt-get update

# Install build tools and ZeroMQ
sudo apt-get install -y build-essential cmake pkg-config
sudo apt-get install -y libzmq3-dev

# Install additional utilities
sudo apt-get install -y git htop iotop net-tools
```

### 2. Deploy Source Code

On both machines:

```bash
# Create project directory
mkdir -p ~/distributed_timestamp
cd ~/distributed_timestamp

# Extract source files from the provided package
unzip distributed_timestamp_system.zip
cd distributed_timestamp_package/src

# Copy configuration files
mkdir -p ~/distributed_timestamp/config
cp ../config/* ~/distributed_timestamp/config/
```

### 3. Build the Software

On both machines:

```bash
# Create build directory
mkdir -p build
cd build

# Configure and build
cmake ..
make

# Verify executables were created
ls -la master_timestamp slave_timestamp

# Build utility programs
g++ -o timestamp_analysis ../timestamp_analysis.cpp -lzmq -pthread
g++ -o network_test ../network_test.cpp -lzmq -pthread

# Make executables available in PATH (optional)
sudo cp master_timestamp slave_timestamp timestamp_analysis network_test /usr/local/bin/
```

## Configuration

### Master Configuration

Create or edit the master configuration file:

```bash
nano ~/distributed_timestamp/config/master_config.json
```

Add the following content (adjust values as needed):

```json
{
  "local_tc_address": "192.168.1.20",
  "slave_address": "192.168.1.11",
  "trigger_port": 5557,
  "status_port": 5559,
  "file_port": 5560,
  "command_port": 5561,
  "sub_duration": 0.2,
  "output_dir": "/home/ubuntu/outputs"
}
```

### Slave Configuration

Create or edit the slave configuration file:

```bash
nano ~/distributed_timestamp/config/slave_config.json
```

Add the following content (adjust values as needed):

```json
{
  "local_tc_address": "192.168.1.21",
  "master_address": "192.168.1.10",
  "trigger_port": 5557,
  "status_port": 5559,
  "file_port": 5560,
  "command_port": 5561,
  "sub_duration": 0.2,
  "output_dir": "/home/ubuntu/outputs",
  "heartbeat_interval_ms": 100
}
```

### Time Controller Configuration

On Master Machine (PC1):
```bash
# Test connection to Time Controller
# Replace with actual command for your Time Controller model
tc_connect --address 192.168.1.20

# Configure Time Controller settings
# These commands will vary based on your specific hardware
tc_config --reference external --trigger manual
```

On Slave Machine (PC2):
```bash
# Test connection to Time Controller
# Replace with actual command for your Time Controller model
tc_connect --address 192.168.1.21

# Configure Time Controller settings
# These commands will vary based on your specific hardware
tc_config --reference external --trigger external
```

### Create Output Directories

On both machines:
```bash
# Create output directory
mkdir -p ~/outputs
```

## Running the System

### 1. Start the Slave Agent

On Slave Machine (PC2):

```bash
# Start the slave agent with configuration file
cd ~/distributed_timestamp/build
./slave_timestamp --config ~/distributed_timestamp/config/slave_config.json

# Alternatively, specify parameters directly
./slave_timestamp \
  --local-tc 192.168.1.21 \
  --master 192.168.1.10 \
  --trigger-port 5557 \
  --status-port 5559 \
  --file-port 5560 \
  --command-port 5561 \
  --output-dir ~/outputs
```

You should see output similar to:
```
Initializing Slave Agent...
Local Time Controller: 192.168.1.21
Master address: 192.168.1.10
Setting up communication channels...
Connecting to local Time Controller...
Local Time Controller identified: [TC identification string]
Slave Agent initialized successfully.
Slave agent initialized and waiting for trigger commands...
Press Ctrl+C to stop
```

### 2. Run the Master Controller

On Master Machine (PC1):

```bash
# Run the master controller with configuration file
cd ~/distributed_timestamp/build
./master_timestamp --config ~/distributed_timestamp/config/master_config.json --duration 0.6 --channels "1,2,3,4"

# Alternatively, specify parameters directly
./master_timestamp \
  --local-tc 192.168.1.20 \
  --slave 192.168.1.11 \
  --trigger-port 5557 \
  --status-port 5559 \
  --file-port 5560 \
  --command-port 5561 \
  --output-dir ~/outputs \
  --duration 0.6 \
  --channels "1,2,3,4"
```

You should see output similar to:
```
Initializing Master Controller...
Local Time Controller: 192.168.1.20
Remote Slave: 192.168.1.11
Setting up communication channels...
Connecting to local Time Controller...
Local Time Controller identified: [TC identification string]
Checking slave availability...
Slave is available and responding.
Master Controller initialized successfully.
Triggering synchronized acquisition for 0.6 seconds...
Preparing for synchronized acquisition...
Sending trigger to slave...
Starting local acquisition...
Acquisition in progress for 0.6 seconds...
Stopping local acquisition...
Waiting for data processing to complete...
Acquisition completed.
Waiting for file transfer to complete...
Receiving file: slave_results_20250527_123456.txt (1048576 bytes in 16 chunks)
File transfer progress: 16/16 chunks (100%)
File reception complete: slave_results_20250527_123456.txt (16/16 chunks)
Master timestamp completed successfully.
```

## Verification and Testing

### 1. Verify Results

On Master Machine (PC1):

```bash
# Check output directory for result files
ls -la ~/outputs/

# Examine the timestamp files
head -n 20 ~/outputs/results_*.txt
head -n 20 ~/outputs/slave_results_*.txt
```

Verify that both files contain timestamp data with picosecond precision headers.

### 2. Calculate Time Delay

Use the timestamp analysis tool to calculate the delay between master and slave timestamps:

```bash
# Run timestamp analysis
cd ~/distributed_timestamp/build
./timestamp_analysis --master ~/outputs/results_*.txt --slave ~/outputs/slave_results_*.txt
```

This will output the calculated delay between the two systems with picosecond precision.

### 3. Network Performance Testing

Run the network test utility to verify communication performance:

On Master Machine (PC1):
```bash
./network_test master 192.168.1.11
```

On Slave Machine (PC2):
```bash
./network_test slave 192.168.1.10
```

Verify that:
- Latency is less than 1ms
- Throughput exceeds 50 MB/s
- No connection errors are reported

### 4. Long-Duration Testing

For stability testing, run multiple acquisitions in sequence:

```bash
# On Master Machine (PC1)
cd ~/distributed_timestamp/build
for i in {1..10}; do
  ./master_timestamp --config ~/distributed_timestamp/config/master_config.json --duration 0.6 --channels "1,2,3,4"
  sleep 5
done

# Analyze results
./timestamp_analysis --directory ~/outputs --statistics
```

## Troubleshooting

### Common Issues and Solutions

#### Connection Problems

**Issue**: Master cannot connect to slave
**Solution**: 
- Verify network connectivity with ping
- Check that slave agent is running
- Ensure firewall allows required ports
- Verify IP addresses are correctly configured

#### Synchronization Issues

**Issue**: Poor timestamp synchronization
**Solution**:
- Verify Time Controllers are properly synchronized to a common reference
- Check network latency between PCs (should be < 1ms for optimal performance)
- Consider using a dedicated network for timestamp traffic
- Verify cables are high quality and properly connected

#### File Transfer Problems

**Issue**: File transfer fails or is incomplete
**Solution**:
- Check available disk space on master machine
- Verify network stability with ping during transfer
- Increase socket buffer sizes if necessary
- Check for firewall or antivirus interference

#### Time Controller Communication Issues

**Issue**: Cannot connect to Time Controller
**Solution**:
- Verify physical connection to Time Controller
- Check Time Controller IP address configuration
- Ensure Time Controller is powered on and initialized
- Check Time Controller documentation for specific connection requirements

### Diagnostic Commands

```bash
# Check ZeroMQ version
dpkg -l | grep zmq

# Check network interface status
ip addr show
ethtool ens33

# Check network connections
netstat -tuln | grep '555[0-9]'

# Check system resources
htop
iotop

# Check disk space
df -h

# Check system logs
journalctl -f
```

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

### Network Tuning

For optimal network performance:

```bash
# Disable TCP slow start
sudo sysctl -w net.ipv4.tcp_slow_start_after_idle=0

# Increase TCP window size
sudo sysctl -w net.ipv4.tcp_window_scaling=1

# Optimize for low latency
sudo sysctl -w net.ipv4.tcp_low_latency=1
```

## Maintenance

### Regular Checks

Perform these checks regularly to ensure system health:

1. Verify network connectivity between master and slave
2. Check Time Controller synchronization
3. Monitor disk space for output files
4. Review system logs for errors
5. Test timestamp synchronization accuracy

### Software Updates

To update the distributed timestamp system:

1. Stop all running instances
2. Backup configuration files
3. Deploy new source code
4. Rebuild the software
5. Restore configuration files
6. Test the system thoroughly before production use

### Backup Procedures

Regularly backup these critical components:

1. Configuration files
2. Custom scripts or modifications
3. Important timestamp data
4. System logs for troubleshooting history

## Conclusion

This comprehensive deployment guide provides all the necessary information to set up, configure, and maintain the distributed timestamp system. By following these instructions, you can achieve picosecond-level synchronization between distributed Time Controllers and reliably transfer timestamp data between systems.

For additional information, refer to the architecture and protocol documentation included in the deployment package.
