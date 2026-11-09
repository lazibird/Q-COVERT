import os
import time
import threading
import paramiko
import subprocess


def execute_commands_queit(instance_name, ip, cmds, private_key_path, user_name, verbose):

    ssh_client = paramiko.SSHClient()
    ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    private_key = paramiko.Ed25519Key.from_private_key_file(private_key_path)

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

                    # stderr
                    if channel.recv_stderr_ready():
                        error_output = channel.recv_stderr(1024).decode('utf-8')
                        print(error_output, end="")
                    
                    # Check if the command is finished
                    if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                        # output = channel.recv(1024).decode('utf-8')  # Adjust buffer size if necessary
                        # print(output, end="")  # Print the output without adding extra newlines
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

def scp_from_remote(hostname, username, key_filepath, remote_filepath, local_directory, port=22):
    # Load SSH private key (assuming you're using key-based authentication)
    private_key = paramiko.Ed25519Key.from_private_key_file(key_filepath)

    # Create SSH client
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(hostname=hostname, username=username, pkey=private_key, port=port)

    # Open SFTP session
    sftp = ssh.open_sftp()

    for f in remote_filepath:
        fname = os.path.basename(f)
        local_filepath = os.path.join(local_directory, fname)
    
        # Download remote file
        sftp.get(f, local_filepath)
    
    # Close connections
    sftp.close()
    ssh.close()

def check_ssh_connection(hostname, username, key_filepath, verbose, timeout=300, interval=5):
    start_time = time.time()
    private_key = paramiko.Ed25519Key.from_private_key_file(key_filepath)

    while time.time() - start_time < timeout:
        ssh_client = paramiko.SSHClient()
        ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

        try:
            ssh_client.connect(hostname, username=username, pkey=private_key, timeout=10)
            if verbose:
                print(f"✅ Successfully connected to {hostname}")
            ssh_client.close()
            return True
        except (paramiko.ssh_exception.NoValidConnectionsError,
                paramiko.AuthenticationException,
                paramiko.ssh_exception.SSHException,
                TimeoutError) as e:
            if verbose:
                print(f"⏳ Waiting for SSH... ({e})")
            time.sleep(interval)

    raise TimeoutError(f"SSH connection to {hostname} failed after {timeout}s. Last error: {last_exception}")
