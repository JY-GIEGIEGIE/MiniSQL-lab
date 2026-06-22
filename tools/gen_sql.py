import random, sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
batch = int(sys.argv[2]) if len(sys.argv) > 2 else 10000

for i in range(n):
    name = f"name{i:05d}"
    balance = round(random.uniform(0, 99999.99), 2)
    print(f"insert into account values({i}, \"{name}\", {balance});")
