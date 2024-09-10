from bitstring import Bits
from netCDF4 import Dataset
import numpy as np
import s3fs
import sys
import os

s3 = s3fs.S3FileSystem(anon=True)
cache_base = '.s3cache'

def unpack_version(version):
    bits = Bits(uint=version, length=32)
    check, year, day, hour, fnum = bits.unpack('uint:2, uint:8, uint:9, uint:5, uint:8')
    if check  == 0:
        minute = -1
    else:
        minute = fnum
        fnum = -1
    if check != 0 and check != 1:
        print(f'WARNING: version check value mismatch. Expected 0 or 1, got {check}.', file=sys.stderr)
    return(1900+year, day, hour, minute, fnum)

def build_noaa_dir(var_name, version):
    var_parts = var_name.split('/')
    product = var_parts[0]
    if product == 'FDCC':
        pname = 'ABI-L2-FDCC'
    elif product == 'RadC':
        pname = 'ABI-L1b-RadC'
    elif product == 'RadM':
        pname = 'ABI-L1b-RadM'
    else:
        raise ValueError(f'ERROR: {product} is not yet implemented.')
    year, day, hour, minute, fnum = unpack_version(version)
    return(f'{pname}/{year}/{day:03d}/{hour:02d}')

def build_noaa_file_base(var_name):
    var_parts = var_name.split('/')
    product = var_parts[0]
    if product == 'FDCC':
        return('OR_ABI-L2-FDCC-M6')
    elif product == 'RadC':
        channel = get_channel(var_parts[1])
        return(f'OR_ABI-L1b-RadC-M6C{channel:02d}')
    elif product == 'RadM':
        zone = var_parts[1]
        channel = get_channel(var_parts[2])
        return(f'OR_ABI-L1b-Rad{zone}-M6C{channel:02d}')

def build_dir_file(name, version):
    nspace, var_name = name.split('\\')
    if nspace == 'goes17':
        return(build_noaa_dir(var_name, version), build_noaa_file_base(var_name))

def get_channel(channel_str):
    if channel_str[0] != 'C':
        print(f'ERROR: {channel_str} does not appear to be a channel.', file=sys.stderr)
        return(-1)
    return(int(channel_str[1:]))

def get_start_time(e):
    start_time = e.split('_')[-3]
    return(int(start_time[1:]))

def query_s3(dir_base, file_base, times):
    files = s3.ls(f's3://noaa-goes17/{dir_base}')
    results = []
    year, day, hour, minute, fcount = times
    time_start = f's{year}{day:03d}{hour:02d}{minute}'
    for f in files:
            if f.startswith(f'noaa-goes17/{dir_base}/{file_base}'):
                if minute > -1 and f.find(time_start) > 0:
                    return(f)
                results.append(f)
    results.sort(key=get_start_time)
    return(results[fcount])


def download_to_cache(s3_fname, fcount):
    os.makedirs(target_dir, exist_ok = True)
    
def build_cache_entry(dir_base, file_base, fcount):
    cache_dir = f'{cache_base}/noaa-goes17/{dir_base}'
    cache_file = f'{file_base}_G17_{fcount}.nc'
    return(f'{cache_dir}/{cache_file}')

def validate(bucket, path):
    if not path.endswith('.nc'):
        raise ValueError("S3 module can only access netCDF files.")

def reg_query(name, version, lb, ub, params, bucket, path):
    if 'var_name' not in params:
        raise ValueError
    var_name = params['var_name']
    cache_entry = f'{cache_base}/{bucket}/{path}'
    if not os.path.exists(cache_entry):
        s3.get(f'{bucket}/{path}', cache_entry)
    data = Dataset(cache_entry)
    array = data[var_name]
    if lb:
        index = [ slice(lb[x], ub[x]+1) for x in range(len(lb)) ]
    else:
        index = [ slice(0, x) for x in array.shape]
    return(array[index])

def query(name, version, lb, ub):
    dir_base, file_base = build_dir_file(name, version)
    times = unpack_version(version)
    fcount = max(times[-2], times[-1])
    centry = build_cache_entry(dir_base, file_base, fcount)
    if not os.path.exists(centry):
        s3_file = query_s3(dir_base, file_base, times)
        print(s3_file)
        s3.get(s3_file, centry)
    var = name.split('/')[-1]
    data = Dataset(centry)
    array = data[var]
    if lb != None:
        index = [ slice(lb[x], ub[x]+1) for x in range(len(lb)) ]
    else:
        index = [ slice(0, x) for x in array.shape ]
    return(array[index])

if __name__ == '__main__':
    var_name = 'goes17\\RadM/M1/C2/Rad'
    version = 505081608
    print(query(var_name, version, (1,1), (4,2)))
