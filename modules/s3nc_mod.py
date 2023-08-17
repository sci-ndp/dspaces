import numpy as np
import sys

def query(name, version, lb, ub):
    dims = np.subtract(ub, lb) + 1
    data = np.random.rand(*dims)
    print(data)
    sys.stdout.flush()
    return(data)

if __name__ == '__main__':
    foo()
