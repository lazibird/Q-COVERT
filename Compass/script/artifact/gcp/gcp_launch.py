# import argparse
# from gcp_utils import *



# delete_instance(project_id, zone, "ae-server")

# create_instance(project_id, zone, server_config, "ae-server")
# create_instance(project_id, zone, server_config, "ae-client")

# delete_instance(project_id, zone, "ae-server")
# delete_instance(project_id, zone, "ae-client")

# # instances_config = [
# #     {
# #         "name": "tracer",
# #         "machine_type": "n2-standard-8",
# #         "boot_disk_image": f"projects/{project_id}/global/images/{image_name}",
# #         "disk_size_gb": 512,  # Optional, defaults to 10GB
# #         "disk_type" :f"zones/{zone}/diskTypes/pd-ssd"
# #     },
# # ]


# # ssh_commands = [
# #     "top -b -n 1",
# #     "pwd",
# #     "ls",
# #     "cd see/ && git status",
# #     "ls",
# #     "echo 'Hello, GCP instance!' > /tmp/test.txt"
# # ]

# # client_commands = [
# #     "cd see/ && git pull",
# #     "cd see/build/ && make",
# #     # "cd see/build/ && ./test_compass_tp r=2 ip=10.128.15.198 d=laion",
# #     # "ls",
# #     # "echo 'Hello, GCP instance!' > /tmp/test.txt"
# # ]

# # if __name__ == "__main__":

# #     parser = argparse.ArgumentParser()

# #     parser.add_argument("-c", "--create", type=int, help='create n tracers')
# #     parser.add_argument("-d", "--delete", action="store_true", help="remove all tracers")
# #     parser.add_argument("-p", "--prepare", action="store_true", help="prepare all tracers for tp")
# #     parser.add_argument("-s", "--server", type=int, help="run remote servers")
# #     parser.add_argument("-l", "--lserver", type=int, help="run local servers")
# #     parser.add_argument("-x", "--exec", type=int, help="run n clients")

# #     args = parser.parse_args()

# #     if args.create:
# #         create_tracers(project_id, zone, instances_config, args.create)
# #     elif args.delete:
# #         delete_tracers(project_id, zone)
# #     elif args.prepare:
# #         execute_prepare_commands(client_commands, private_key_path, user_name)
# #     elif args.server:
# #         run_remote_servers(args.server)
# #     elif args.lserver:
# #         run_local_servers(args.lserver)
# #     elif args.exec:
# #         run_remote_clients(args.exec)





# # list_instances(project_id)
# # exit()

# # create_instances(project_id, zone, instances_config)
# # exit()

# # create_tracers(project_id, zone, instances_config, 2)
# # exit()

# # delete_tracers(project_id, zone)
# # exit()

# # execute_prepare_commands(client_commands, private_key_path, user_name)
# # exit()


# # instances = get_instances(project_id)

# # # filter 

# # ips = []
# # tmp_instances = []
# # for instance in instances:
# #     if "tracer" in instance["name"]:
# #         tmp_instances.append(instance)
# #         ips.append(instance["internal_ip"])

# # print(ips)

# # execute_commands_on_instances(tmp_instances, client_commands, private_key_path, user_name)