#!/bin/bash

# Set default values for parameters
DEFAULT_R=1
# DEFAULT_IP="10.128.15.198"
DEFAULT_PORT=9000
DEFAULT_D="laion"

# Check if the number of runs is provided as a command line argument
if [ $# -lt 2 ]; then
    echo "Usage: $0 NUM_RUNS IP_ADDRESS"
    exit 1
fi

NUM_RUNS=$1
IP_ADDRESS=$2

ps aux | grep compass | grep -v grep | awk '{print $2}' | xargs kill

# Change working directory to ~/compass/build
cd ~/compass/build || { echo "Error: Directory ~/compass/build does not exist."; exit 1; }

# Loop to execute the program multiple times in the background
for ((i=0; i<NUM_RUNS; i++))
do
    PORT=$((DEFAULT_PORT + 2 * i))
    echo "Starting run $i with port $PORT in background..."
    ./test_compass_tp r=$DEFAULT_R ip=$IP_ADDRESS p=$PORT d=$DEFAULT_D > /dev/null 2>&1 &
    
    # Save the process ID (PID) of the background job
    PID=$!
    echo "Run $i started with PID $PID."
done

echo "All runs started in the background."
