
import time
import threading
import paramiko
import subprocess

from google.cloud import compute_v1


project_id = "encrypted-search-404320"
zone = "us-central1-c"
vpc_name = "skypilot-vpc"
image_name = "artifact"
private_key_path = "~/.ssh/compass_artifact"
user_name = "compass"

port = 9000
server_ip = "10.128.15.198"
dataset = "laion"

throuput = []


def list_instances(project_id):
    """
    List all Compute Engine instances in the given project.

    :param project_id: Your GCP project ID
    """
    instance_client = compute_v1.InstancesClient()
    aggregated_list = instance_client.aggregated_list(project=project_id)

    # Iterate through the aggregated list
    for zone, scoped_list in aggregated_list:
        # Check if there are instances in this zone
        if scoped_list.instances:
            print(f"Zone: {zone}") 
            for instance in scoped_list.instances:

                internal_ip = None
                external_ip = None

                for network_interface in instance.network_interfaces:
                    internal_ip = network_interface.network_i_p
                    if network_interface.access_configs:
                        external_ip = network_interface.access_configs[0].nat_i_p


                print(f"  - Instance Name: {instance.name}")
                print(f"    Status: {instance.status}")
                print(f"    Machine Type: {instance.machine_type.split('/')[-1]}")
                print(f"    Internal IP: {internal_ip}")
                print(f"    External IP: {external_ip}")
                # print(f"    Network Interfaces: {instance.network_interfaces}")
                # print(f"    Disks: {instance.disks}\n")
        else:
            # print(f"Zone: {zone} has no instances.")
            continue

def get_instances(project_id):
    """
    List all Compute Engine instances in the given project.

    :param project_id: Your GCP project ID
    """
    instance_client = compute_v1.InstancesClient()
    aggregated_list = instance_client.aggregated_list(project=project_id)

    instances_list = []

    # Iterate through the aggregated list
    for zone, scoped_list in aggregated_list:
        # Check if there are instances in this zone
        if scoped_list.instances:
            print(f"Zone: {zone}") 
            for instance in scoped_list.instances:

                metadata = {}

                internal_ip = None
                external_ip = None

                for network_interface in instance.network_interfaces:
                    internal_ip = network_interface.network_i_p
                    if network_interface.access_configs:
                        external_ip = network_interface.access_configs[0].nat_i_p

                metadata["name"] = instance.name
                metadata["status"] = instance.status
                metadata["type"] = instance.machine_type.split('/')[-1]
                metadata["internal_ip"] = internal_ip
                metadata["external_ip"] = external_ip
                
                instances_list.append(metadata)
        else:
            # print(f"Zone: {zone} has no instances.")
            continue

    return instances_list


def create_instances(project_id, zone, instances_config):
    """
    Create multiple Compute Engine instances.

    :param project_id: Your GCP project ID
    :param zone: The zone where the instances will be created
    :param instances_config: List of dictionaries with instance configurations
    """
    instance_client = compute_v1.InstancesClient()
    operation_client = compute_v1.ZoneOperationsClient()

    for config in instances_config:
        instance = compute_v1.Instance()
        instance.name = config['name']
        instance.machine_type = f"zones/{zone}/machineTypes/{config['machine_type']}"

        # Configure boot disk
        initialize_params = compute_v1.AttachedDiskInitializeParams(
            source_image=config['boot_disk_image'],
            disk_size_gb=config.get('disk_size_gb', 10)  # Default 10GB
        )
        boot_disk = compute_v1.AttachedDisk(
            boot=True, auto_delete=True, initialize_params=initialize_params
        )
        instance.disks = [boot_disk]

        # Configure network
        network_interface = compute_v1.NetworkInterface()
        network_interface.network = f"projects/{project_id}/global/networks/{vpc_name}"

        # Add access configuration to enable external IP
        access_config = compute_v1.AccessConfig(
            name="External NAT", type_="ONE_TO_ONE_NAT"
        )
        network_interface.access_configs = [access_config]

        
        instance.network_interfaces = [network_interface]

        # Create instance
        operation = instance_client.insert(
            project=project_id, zone=zone, instance_resource=instance
        )

        # Wait for the operation to complete
        operation_client.wait(project=project_id, zone=zone, operation=operation.name)
        print(f"Instance {config['name']} created successfully.")


def create_instance(project_id, zone, config, instance_name):

    print(f"Creating instance: {instance_name}.")
    
    instance_client = compute_v1.InstancesClient()
    operation_client = compute_v1.ZoneOperationsClient()

    instance = compute_v1.Instance()
    instance.name = instance_name
    instance.machine_type = f"zones/{zone}/machineTypes/{config['machine_type']}"

    # Configure boot disk
    initialize_params = compute_v1.AttachedDiskInitializeParams(
        source_image=config['boot_disk_image'],
        disk_size_gb=config.get('disk_size_gb', 10)  # Default 10GB
    )
    boot_disk = compute_v1.AttachedDisk(
        boot=True, auto_delete=True, initialize_params=initialize_params
    )
    instance.disks = [boot_disk]

    # Configure network
    network_interface = compute_v1.NetworkInterface()
    network_interface.network = f"projects/{project_id}/global/networks/{vpc_name}"

    # Add access configuration to enable external IP
    access_config = compute_v1.AccessConfig(
        name="External NAT", type_="ONE_TO_ONE_NAT"
    )
    network_interface.access_configs = [access_config]

    
    instance.network_interfaces = [network_interface]

    # Create instance
    operation = instance_client.insert(
        project=project_id, zone=zone, instance_resource=instance
    )

    # Wait for the operation to complete
    operation_client.wait(project=project_id, zone=zone, operation=operation.name)
    print(f"Instance {instance_name} created successfully.")

def create_tracers(project_id, zone, instances_config, num_instances):

    instances = get_instances(project_id)

    tracers = []

    for instance in instances:
        if "tracer" in instance["name"]:
            raise AssertionError()
    
    tracers = []
    for i in range(num_instances):
        tracers.append(instances_config[0]["name"] + "-" + str(i))

    threads = []

    for tracer_name in tracers:
        thread = threading.Thread(target=create_instance, args=(project_id, zone, instances_config[0], tracer_name))
        threads.append(thread)
        thread.start()

    # Wait for all threads to finish
    for thread in threads:
        thread.join()




def delete_instance(project_id, zone, instance_name):
    """Deletes a GCP instance."""
    instance_client = compute_v1.InstancesClient()

    try:
        # Make the API request to delete the instance
        operation = instance_client.delete(
            project=project_id,
            zone=zone,
            instance=instance_name,
        )
        print(f"Deleting instance {instance_name}: {operation.operation_type}")

        # Wait for the operation to complete
        operation_client = compute_v1.ZoneOperationsClient()
        operation_client.wait(project=project_id, zone=zone, operation=operation.name)
        print(f"Instance {instance_name} deleted successfully.")
    except Exception as e:
        print(f"Failed to delete instance {instance_name}: {e}")


def delete_tracers(project_id, zone):

    instances = get_instances(project_id)

    tracers = []

    for instance in instances:
        if "tracer" in instance["name"]:
            tracers.append(instance["name"])

    print(tracers)

    threads = []

    for tracer_name in tracers:
        thread = threading.Thread(target=delete_instance, args=(project_id, zone, tracer_name))
        threads.append(thread)
        thread.start()

    # Wait for all threads to finish
    for thread in threads:
        thread.join()
    


def execute_commands_on_instances(instances, cmds, private_key_path, user_name):

    for instance in instances:

        ssh_client = paramiko.SSHClient()
        ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

        private_key = paramiko.RSAKey.from_private_key_file(private_key_path)

        try:
            print(f"Try connection")
            ssh_client.connect(hostname=instance["internal_ip"], username=user_name, pkey=private_key)
            print(f"Connected to {instance['name']} at {instance['internal_ip']}")

            for cmd in cmds:

                print("exec_command: ", cmd)

                transport = ssh_client.get_transport()
                channel = transport.open_session()
                channel.exec_command(cmd)

                # Execute a command
                # command = "echo 'Hello, GCP instance!' > /tmp/test.txt"
                # stdin, stdout, stderr = ssh_client.exec_command(cmd)

                # exit_status = stdout.channel.recv_exit_status()

                # print("Exit Status:", exit_status)

                # # Print command output
                # print("stdout:", stdout.read().decode('utf-8'))
                # print("stderr:", stderr.readlines())

                # Read the stdout in real-time
                while True:
                    # Check if data is ready to be read
                    if channel.recv_ready():
                        output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        print(output, end="")  # Print the output without adding extra newlines
                    
                    # Check if the command is finished
                    if channel.exit_status_ready():
                        output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        print(output, end="")  # Print the output without adding extra newlines
                        break

                    # Prevent high CPU usage in the loop
                    time.sleep(0.1)
                
                print("\n")

            # Close the connection
            ssh_client.close()
            print("Connection closed.")
        except Exception as e:
            print(f"Error: {e}")


def execute_commands_queit(instance_name, ip, cmds, private_key_path, user_name, verbose):

    ssh_client = paramiko.SSHClient()
    ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    private_key = paramiko.RSAKey.from_private_key_file(private_key_path)

    try:
        if verbose:
            print(f"Try to connect to: {instance_name}")

        ssh_client.connect(hostname=ip, username=user_name, pkey=private_key)
        if verbose:
            print(f"Connected to {instance_name} at {ip}")

        for cmd in cmds:

            if verbose:
                print("exec_command: ", cmd)

            transport = ssh_client.get_transport()
            channel = transport.open_session()
            channel.exec_command(cmd)

            if verbose:
                # Read the stdout in real-time
                while True:
                    # Check if data is ready to be read
                    if channel.recv_ready():
                        output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        print(output, end="")  # Print the output without adding extra newlines
                    
                    # Check if the command is finished
                    if channel.exit_status_ready():
                        output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        print(output, end="")  # Print the output without adding extra newlines
                        break

                    # Prevent high CPU usage in the loop
                    time.sleep(0.1)
            else:
                # wait until the cmd is complete
                while not channel.exit_status_ready():
                    time.sleep(0.1)

        if verbose:
            print("\n")

        # Close the connection
        ssh_client.close()
    except Exception as e:
        print(f"Error: {e}")


def execute_prepare_commands(cmds, private_key_path, user_name):

    print("execute prepare commands: ")
    instances = get_instances(project_id)

    tracers = []
    tracer_ips = []

    for instance in instances:
        if "tracer" in instance["name"]:
            tracers.append(instance["name"])
            tracer_ips.append(instance["internal_ip"])

    print(tracers)

    threads = []

    for tracer, ip in zip(tracers, tracer_ips):
        thread = threading.Thread(target=execute_commands_queit, args=(tracer, ip, cmds, private_key_path, user_name, True))
        threads.append(thread)
        thread.start()

    # Wait for all threads to finish
    for thread in threads:
        thread.join()

    print("execute prepare commands: -> done")


def run_local_command(cmd):
    try:
        print(cmd.split())
        result = subprocess.Popen(cmd.split(),  cwd="/home/clive/see/build/", stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        # Capture output and errors
        stdout, stderr = result.communicate()

        # Print output and errors
        print("Output:", stdout.decode())
        print("Errors:", stderr.decode())

        # print(f"Command: {cmd}\nOutput:\n{result.stdout}\nError:\n{result.stderr}")
    except Exception as e:
        print(f"Error running command '{cmd}': {e}")

def run_local_servers(n_servers):

    instances = get_instances(project_id)

    tracers = []

    for instance in instances:
        if "tracer" in instance["name"]:
            tracers.append(instance["name"])
    
    if len(tracers) == 0:
        print("No active tracers.")
        return
    
    if len(tracers) < n_servers:
        assert 0

    cmds = []
    for i in range(n_servers):
        cmd = f"./test_compass_tp r=1 ip={server_ip} p={port+2*i} d={dataset}"
        cmds.append(cmd)

    threads = []
    
    for cmd in cmds:
        thread = threading.Thread(target=run_local_command, args=(cmd,))
        threads.append(thread)
        thread.start()

    for thread in threads:
        thread.join()


def run_remote_servers(n_servers):

    instances = get_instances(project_id)

    instance_name = "eval-base"
    ip = None

    tracers = []


    for instance in instances:
        if instance_name == instance["name"]:
            ip = instance["internal_ip"]
        if "tracer" in instance["name"]:
            tracers.append(instance["name"])
        
    
    if ip == None:
        assert 0

    if len(tracers) < n_servers:
        assert 0


    cmds = []
    for i in range(n_servers):
        cmd = f"cd /home/clive/see/build/ && ./test_compass_tp r=1 ip={server_ip} p={port+2*i} d={dataset}"
        cmds.append(cmd)

    threads = []

    reset_cmd = "ps aux | grep compass | grep -v grep | awk '{print $2}' | xargs kill"
    
    for cmd in cmds:
        thread = threading.Thread(target=execute_commands_reduce, args=(instance_name, ip, [reset_cmd, cmd], private_key_path, user_name, False, False))
        threads.append(thread)
        thread.start()

    for thread in threads:
        thread.join()


def execute_commands_reduce(instance_name, ip, cmds, private_key_path, user_name, verbose, log_tp):

    ssh_client = paramiko.SSHClient()
    ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    private_key = paramiko.RSAKey.from_private_key_file(private_key_path)

    if log_tp:
        id = int(instance_name.split('-')[-1])

    try:
        if verbose:
            print(f"Try to connect to: {instance_name}")

        ssh_client.connect(hostname=ip, username=user_name, pkey=private_key)
        if verbose:
            print(f"Connected to {instance_name} at {ip}")

        for cmd in cmds:

            if verbose:
                print("exec_command: ", cmd)

            transport = ssh_client.get_transport()
            channel = transport.open_session()
            channel.exec_command(cmd)

            if verbose:
                # Read the stdout in real-time
                while True:
                    # Check if data is ready to be read
                    if channel.recv_ready():
                        output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        print(f"[{instance_name}]: ", output, end="")  # Print the output without adding extra newlines
                    
                    # Check if the command is finished
                    if channel.exit_status_ready():
                        output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        print(f"[{instance_name}]: ",output, end="")  # Print the output without adding extra newlines
                        break

                    # Prevent high CPU usage in the loop
                    time.sleep(0.1)
            else:
                # wait until the cmd is complete
                while True:
                    # Check if data is ready to be read
                    if log_tp:
                        if channel.recv_ready():
                            output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                            if output.split(" ")[0] == "Throughput:":
                                tp = float(output.split(" ")[1])
                                # print("assign to id: ", id, " length of trhouput: ", len(throuput))
                                throuput[id] = tp
                    
                    # Check if the command is finished
                    if channel.exit_status_ready():
                        # output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        # print(f"[{instance_name}]: ",output, end="")  # Print the output without adding extra newlines
                        break

                    # Prevent high CPU usage in the loop
                    time.sleep(0.1)

        if verbose:
            print("\n")

        # Close the connection
        ssh_client.close()
    except Exception as e:
        print(f"Error: {e}")

def tp_monitor():
    while True:
        total_tp = 0
        for tp in throuput:
            total_tp += tp
        print("Total throughput: ", total_tp)
        time.sleep(1)

def run_remote_clients(n_clients):

    print("run remote client: ")
    instances = get_instances(project_id)

    tracers = []
    tracer_ips = []

    for instance in instances:
        if "tracer" in instance["name"]:
            tracers.append(instance["name"])
            tracer_ips.append(instance["internal_ip"])
    
    if len(tracers) == 0:
        print("No active tracers.")
        return

    if len(tracers) < n_clients:
        print("Insufficient active tracers.")
        return

    global throuput
    throuput = [0] * len(tracers)

    tracers = tracers[0:n_clients]
    tracer_ips = tracer_ips[0:n_clients]

    cmds = []
    for i in range(len(tracers)):
        cmd = f"cd /home/clive/see/build/ && ./test_compass_tp r=2 ip={server_ip} p={port+2*i} d={dataset}"
        cmds.append(cmd)

    threads = []


    reset_cmd = "ps aux | grep compass | grep -v grep | awk '{print $2}' | xargs kill"
    
    for tracer, ip, cmd in zip(tracers, tracer_ips, cmds):
        thread = threading.Thread(target=execute_commands_reduce, args=(tracer, ip, [reset_cmd, cmd], private_key_path, user_name, False, True))
        threads.append(thread)
        thread.start()

    monitor_thread = threading.Thread(target=tp_monitor, args=())
    threads.append(monitor_thread)
    monitor_thread.start()

    for thread in threads:
        thread.join()