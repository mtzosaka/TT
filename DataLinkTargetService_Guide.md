# Complete Guide: DataLinkTargetService and Timestamp Project Setup for Ubuntu

This comprehensive guide provides step-by-step instructions for:
1. Installing and configuring DataLinkTargetService on Ubuntu
2. Building and running the timestamp project

## PART 1: DataLinkTargetService Setup

### Prerequisites
- Ubuntu 22.04 or compatible Linux distribution
- Administrative (sudo) privileges

### Step 1: Install Required Dependencies

```bash
# Update package lists
sudo apt-get update

# Install build tools and required libraries
sudo apt-get install -y build-essential cmake
sudo apt-get install -y libzmq5 libpugixml1v5
sudo apt-get install -y libboost-filesystem1.74.0 libboost-program-options1.74.0
sudo apt-get install -y liblog4cplus-dev
```

### Step 2: Install log4cplus from Source (if liblog4cplus-dev is unavailable)

```bash
# Install build dependencies
sudo apt-get install -y build-essential cmake

# Download and extract log4cplus
mkdir -p ~/build
cd ~/build
wget https://sourceforge.net/projects/log4cplus/files/log4cplus-stable/1.2.2/log4cplus-1.2.2.tar.xz/download -O log4cplus-1.2.2.tar.xz
tar -xf log4cplus-1.2.2.tar.xz
cd log4cplus-1.2.2

# Build and install
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Step 3: Create Symbolic Links for Libraries

```bash
# Create symbolic links for log4cplus
sudo ln -sf /usr/lib/x86_64-linux-gnu/liblog4cplus.so.1.2.2 /usr/lib/x86_64-linux-gnu/liblog4cplus-1.2.so.5

# Create symbolic links for Boost libraries
sudo ln -sf /usr/lib/x86_64-linux-gnu/libboost_filesystem.so.1.74.0 /usr/lib/x86_64-linux-gnu/libboost_filesystem.so.1.71.0
sudo ln -sf /usr/lib/x86_64-linux-gnu/libboost_program_options.so.1.74.0 /usr/lib/x86_64-linux-gnu/libboost_program_options.so.1.71.0

# Update the dynamic linker cache
sudo ldconfig
```

### Step 4: Set Up DataLinkTargetService Configuration

```bash
# Create the required directory
sudo mkdir -p /etc/elvis

# Copy the DataLinkTargetService executable
sudo cp DataLinkTargetService /etc/elvis/
sudo chmod +x /etc/elvis/DataLinkTargetService

# Copy configuration files
sudo cp DataLinkTargetService.conf /etc/elvis/
sudo cp DataLinkTargetService.log.conf /etc/elvis/
```

### Step 5: Test DataLinkTargetService

```bash
# Create a data directory
mkdir -p ~/data

# Run DataLinkTargetService
/etc/elvis/DataLinkTargetService -f ~/data
```

If successful, you should see output similar to:
```
Configuration file: /etc/elvis/DataLinkTargetService.conf
Log configuration file: /etc/elvis/DataLinkTargetService.log
Address: tcp://127.0.0.1
Port: 6060
Folder: data
```

## PART 2: Timestamp Project Setup

### Step 1: Install Additional Dependencies

```bash
# Install ZeroMQ development package
sudo apt-get install -y libzmq3-dev
```

### Step 2: Extract and Prepare the Project

```bash
# Extract the project files
unzip timestamp_project.zip -d ~/timestamp_project
cd ~/timestamp_project

# Create build directory
mkdir -p build
cd build
```

### Step 3: Configure and Build the Project

```bash
# Configure with CMake
cmake ..

# Build the project
make
```

### Step 4: Prepare Runtime Environment

```bash
# Create output directory
mkdir -p outputs

# Copy data files if needed
cp -r ../data .
```

### Step 5: Run the Timestamp Client

```bash
# Basic usage
./timestamp_client --output-file outputs/results.txt

# With additional parameters
./timestamp_client --address 169.254.218.109 --channels "1,2,3,4" --duration 0.6 --output-file outputs/results.txt
```

## File Locations

### DataLinkTargetService Files
- Executable: `/etc/elvis/DataLinkTargetService`
- Configuration: `/etc/elvis/DataLinkTargetService.conf`
- Log Configuration: `/etc/elvis/DataLinkTargetService.log.conf`

### Timestamp Project Files
- Source Code: `~/timestamp_project/`
- Build Directory: `~/timestamp_project/build/`
- Executable: `~/timestamp_project/build/timestamp_client`
- Output Files: `~/timestamp_project/build/outputs/`

## Troubleshooting

### Common Issues with DataLinkTargetService

1. **Missing Libraries**
   - Error: `error while loading shared libraries: libXXX.so.X: cannot open shared object file`
   - Solution: Ensure all required libraries are installed and symbolic links are created correctly

2. **Configuration File Not Found**
   - Error: `Configuration file not found`
   - Solution: Verify that configuration files exist in `/etc/elvis/` and have correct permissions

3. **Permission Issues**
   - Error: `Permission denied`
   - Solution: Ensure the executable has execute permissions (`chmod +x`)

### Common Issues with Timestamp Client

1. **Connection Timeout**
   - Symptom: Client appears to hang with no output
   - Cause: The client is waiting for a connection to a Time Controller device
   - Solution: This is expected behavior when the hardware is not present

2. **Output Directory Issues**
   - Error: `Output folder does not exist`
   - Solution: Create the output directory before running the client

## Additional Notes

- The timestamp_client is designed to communicate with specialized hardware (Time Controller)
- When running without this hardware, it will wait for a connection indefinitely
- For testing purposes, you may want to modify the source code to add a timeout or mock the hardware responses
