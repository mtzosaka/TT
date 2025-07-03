# Dual-Machine Deployment and Testing Guide

## Overview

This document provides detailed instructions for deploying and testing the distributed timestamp system on two physical machines. It covers hardware setup, network configuration, software installation, and verification procedures.

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

## Network Configuration

### 1. Physical Connection Setup

1. Connect both PCs to the same Ethernet switch
2. Connect each Time Controller to its respective PC
3. Ensure all connections are secure and using quality cables

### 2. IP Address Configuration

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

### 3. Verify Network Connectivity

On Master Machine (PC1):
```bash
# Ping the Slave Machine
ping 192.168.1.11
```

On Slave Machine (PC2):
```bash
# Ping the Master Machine
ping 192.168.1.10
```

### 4. Network Optimization

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

### 5. Firewall Configuration

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

## Software Installation

### 1. Install Dependencies

On both Master and Slave machines:

```bash
# Update package lists
sudo apt-get update

# Install build tools and ZeroMQ
sudo apt-get install -y build-essential cmake pkg-config
sudo apt-get install -y libzmq3-dev
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
```

## Time Controller Configuration

### 1. Connect to Time Controllers

On Master Machine (PC1):
```bash
# Test connection to Time Controller
# Replace with actual command for your Time Controller model
tc_connect --address 192.168.1.20
```

On Slave Machine (PC2):
```bash
# Test connection to Time Controller
# Replace with actual command for your Time Controller model
tc_connect --address 192.168.1.21
```

### 2. Synchronize Time Controllers

For picosecond-level synchronization:

1. Connect both Time Controllers to a common reference clock
2. Configure Time Controllers to use the same reference source
3. Verify synchronization using the Time Controller's built-in tools

## Deployment Testing

### 1. Start the Slave Agent

On Slave Machine (PC2):

```bash
# Create output directory
mkdir -p ~/outputs

# Start the slave agent
cd ~/distributed_timestamp/distributed_timestamp_package/src/build
./slave_timestamp --local-tc 192.168.1.21 --master 192.168.1.10 --output-dir ~/outputs
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
# Create output directory
mkdir -p ~/outputs

# Run the master controller
cd ~/distributed_timestamp/distributed_timestamp_package/src/build
./master_timestamp --local-tc 192.168.1.20 --slave 192.168.1.11 --output-dir ~/outputs --duration 0.6 --channels "1,2,3,4"
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

### 3. Verify Results

On Master Machine (PC1):

```bash
# Check output directory for result files
ls -la ~/outputs/

# Examine the timestamp files
head -n 20 ~/outputs/results_*.txt
head -n 20 ~/outputs/slave_results_*.txt
```

Verify that both files contain timestamp data with picosecond precision headers.

### 4. Calculate Time Delay

Use the provided timestamp analysis tool to calculate the delay between master and slave timestamps:

```bash
# Run timestamp analysis
cd ~/distributed_timestamp/distributed_timestamp_package/src/build
./timestamp_analysis --master ~/outputs/results_*.txt --slave ~/outputs/slave_results_*.txt
```

This will output the calculated delay between the two systems with picosecond precision.

## Performance Testing

### 1. Network Latency Test

On Master Machine (PC1):

```bash
# Compile the network test utility
cd ~/distributed_timestamp/distributed_timestamp_package/src/build
g++ -o network_test ../network_test.cpp -lzmq -pthread

# Run as master
./network_test master 192.168.1.11
```

On Slave Machine (PC2):

```bash
# Compile the network test utility
cd ~/distributed_timestamp/distributed_timestamp_package/src/build
g++ -o network_test ../network_test.cpp -lzmq -pthread

# Run as slave
./network_test slave 192.168.1.10
```

### 2. Throughput Test

The network test utility will automatically run throughput tests. Verify that the throughput exceeds 50 MB/s for optimal file transfer performance.

### 3. Synchronization Accuracy Test

Run multiple acquisitions and analyze the timestamp consistency:

```bash
# On Master Machine (PC1)
cd ~/distributed_timestamp/distributed_timestamp_package/src/build
for i in {1..10}; do
  ./master_timestamp --local-tc 192.168.1.20 --slave 192.168.1.11 --output-dir ~/outputs --duration 0.6 --channels "1,2,3,4"
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

## Conclusion

This dual-machine deployment and testing guide provides comprehensive instructions for setting up the distributed timestamp system on two physical machines. By following these steps, you can achieve picosecond-level synchronization between distributed Time Controllers and reliably transfer timestamp data between systems.

The next steps after successful deployment are:
1. Integrate the system into your production environment
2. Set up monitoring for long-term stability
3. Implement regular synchronization verification procedures
