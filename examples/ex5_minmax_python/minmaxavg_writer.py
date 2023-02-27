import dspaces as ds
import numpy as np
from mpi4py import MPI
import sys

comm = MPI.COMM_WORLD
rank = comm.Get_rank()
size = comm.Get_size() 

# Get global array size from cli argument
array_size = int(sys.argv[1])
local_size = int(array_size / size)

if len(sys.argv) != 2:
    print(f"Usage: {argv[0]} <array size>")

if array_size % size != 0:
    if rank == 0:
        print("rank count must evenly divide array size.")
        exit()

local_size = int(array_size / size)
# Initialize DataSpaces library - defaults to using COMM_WORLD
client = ds.DSpaces()

num_ts = 3

for ts in range(num_ts):
    data = np.random.randint(low=0, high=65535, size=(local_size))
    local_min = np.ndarray(1, dtype=int)
    local_max = np.ndarray(1, dtype=int)
    local_sum = np.ndarray(1, dtype=int)
    local_min[0] = data.min()
    local_max[0] = data.max()
    local_sum[0] = data.sum()
    # Write data uniformly into global domain
    client.Put(data, "ex5_sample_data", version=ts, offset = ((local_size * rank),))
    global_min = np.ndarray(1, dtype=int)
    global_max = np.ndarray(1, dtype=int)
    global_sum = np.ndarray(1, dtype=int)
    comm.Reduce(local_min, global_min, MPI.MIN, 0)
    comm.Reduce(local_max, global_max, MPI.MAX, 0)
    comm.Reduce(local_sum, global_sum, MPI.SUM, 0)
    if rank == 0:
        avg = float(global_sum[0]) / float(array_size)
        print(f"Written timestep {ts} Max: {global_max}, Min: {global_min}, Average: {avg}")

# Signal server to shutdown
if rank == 0:
    client.KillServer()

