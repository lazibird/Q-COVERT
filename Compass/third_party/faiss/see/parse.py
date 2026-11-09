import re
import matplotlib.pyplot as plt

path = "/home/gcpuser/clive/faiss/build/demos/trip_more.log"

steps = {}
cnt_visited = {}

cnt = 0

with open(path, 'r') as file:
    for line in file:
        entries = line.split(" ")
        for entry in entries:
            if '$' in entry:
                cnt += 1
                level = int(entry.split('$')[1])
                step = int(entry.split('$')[2])
                if level in steps:
                    steps[level].append(step)
                else:
                    steps[level] = [step]
            elif '%' in entry:
                level = 0
                step = int(entry.replace('%', ''))
                if level in steps:
                    steps[level].append(step)
                else:
                    steps[level] = [step]
            elif '?' in entry:
                cnt += 1
                level = int(entry.split('?')[1])
                nodes = int(entry.split('?')[2])
                if level in cnt_visited:
                    cnt_visited[level].append(nodes)
                else:
                    cnt_visited[level] = [step]
            elif '&' in entry:
                level = 0
                nodes = int(entry.replace('&', ''))
                if level in cnt_visited:
                    cnt_visited[level].append(nodes)
                else:
                    cnt_visited[level] = [nodes]


for l in steps:

    plt.hist(steps[l], bins=list(range(min(steps[l]), max(steps[l]) + 5)) ,edgecolor='k', alpha=0.75)
    # print(len(list(range(min(steps[l]), max(steps[l]) + 1))))
    plt.savefig('trip_step_' + str(l)  + '.png')
    plt.clf()

for l in cnt_visited:
    plt.hist(cnt_visited[l], bins=list(range(min(cnt_visited[l]), max(cnt_visited[l]) + 5)), edgecolor='k', alpha=0.75)
    plt.savefig('trip_nodes_' + str(l)  + '.png')
    plt.clf()

print("total: ", cnt)


