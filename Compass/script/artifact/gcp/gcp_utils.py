import time
import threading
import paramiko
import subprocess
from concurrent.futures import ThreadPoolExecutor

from google.cloud import compute_v1


def create_instance(config, instance_name):

    print(f"Creating instance: {instance_name}.")
    
    instance_client = compute_v1.InstancesClient()
    operation_client = compute_v1.ZoneOperationsClient()

    instance = compute_v1.Instance()
    instance.name = instance_name
    instance.machine_type = f"zones/{config['zone']}/machineTypes/{config['machine_type']}"

    # Configure boot disk
    initialize_params = compute_v1.AttachedDiskInitializeParams(
        source_image=config['boot_disk_image'],
        disk_size_gb=config.get('disk_size_gb', 10), 
        disk_type=config["disk_type"]
    )
    boot_disk = compute_v1.AttachedDisk(
        boot=True, auto_delete=True, initialize_params=initialize_params
    )
    instance.disks = [boot_disk]

    # Configure network
    network_interface = compute_v1.NetworkInterface()
    network_interface.network = f"projects/{config['project_id']}/global/networks/{config['vpc_name']}"

    # Add access configuration to enable external IP
    access_config = compute_v1.AccessConfig(
        name="External NAT", type_="ONE_TO_ONE_NAT"
    )
    network_interface.access_configs = [access_config]

    
    instance.network_interfaces = [network_interface]

    # Create instance
    operation = instance_client.insert(
        project=config['project_id'], zone=config['zone'], instance_resource=instance
    )

    # Wait for the operation to complete
    operation_client.wait(project=config['project_id'], zone=config['zone'], operation=operation.name)

    # Retrieve the created instance details
    created_instance = instance_client.get(project=config['project_id'], zone=config['zone'], instance=instance_name)

    internal_ip = created_instance.network_interfaces[0].network_i_p
    # instance_internal_ips[instance_name] = internal_ip

    print(f"Instance {instance_name} created successfully with internal IP: {internal_ip}")

    instance = {
        "name": instance_name,
        "internal_ip": internal_ip
    }

    return instance

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


def create_tracers(config, num_instances):

    tracers = [f"ae-throughput-tracer-{i}" for i in range(num_instances)]

    results = []
    with ThreadPoolExecutor(max_workers=num_instances) as executor:
        futures = [executor.submit(create_instance, config, tracer_name) for tracer_name in tracers]

        for future in futures:
            results.append(future.result())  # this collects return values from create_instance

    return results

def delete_tracers(project_id, zone, tracer_names):

    threads = []

    for name in tracer_names:
        thread = threading.Thread(target=delete_instance, args=(project_id, zone, name))
        threads.append(thread)
        thread.start()

    # Wait for all threads to finish
    for thread in threads:
        thread.join()

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
                # metadata["status"] = instance.status
                # metadata["type"] = instance.machine_type.split('/')[-1]
                metadata["internal_ip"] = internal_ip
                # metadata["external_ip"] = external_ip
                
                instances_list.append(metadata)
        else:
            # print(f"Zone: {zone} has no instances.")
            continue

    return instances_list
