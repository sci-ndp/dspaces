import planetary_computer
from netCDF4 import Dataset
import fsspec
import pystac_client
import numpy as np
import os
from urllib.parse import urlparse
from urllib.request import urlretrieve
from bitstring import Bits, pack
from datetime import date, timedelta
import sys

base_date = date(1950, 1, 1)
present_date = date(2015, 1, 1)
last_date = date(2100, 12, 31)
cache_base='.azrcache'

try:
    catalog = pystac_client.Client.open(
        "https://planetarycomputer.microsoft.com/api/stac/v1",
        modifier=planetary_computer.sign_inplace,
    )
    collection = catalog.get_collection("nasa-nex-gddp-cmip6")
    variable_list=collection.summaries.get_list("cmip6:variable")
    model_list=collection.summaries.get_list("cmip6:model")[:10]
    scenario_list=collection.summaries.get_list("cmip6:scenario")
    have_pc = True
except pystac_client.exceptions.APIError:
    print("don't have planetary computer api access")
    have_pc = False

def _get_gddp_time_ranges(version):
    bits = Bits(uint=version, length=32)
    start_day, span_days = bits.unpack('uint:16, uint:16')
    start_date = base_date + timedelta(days = start_day)
    end_date = start_date + timedelta(days = span_days)
    if start_date > last_date:
        raise ValueError(f'WARNING: start_date of {start_date} is too far in the future')
    if end_date > last_date:
        print(f"WARNING: truncating end_date of {end_date} to {last_date}")
        end_date = last_date
    return start_date, end_date

def _get_gddp_params(name):
    model = 'CESM2'
    scenario = 'ssp585'
    variable = None
    var_name = name.split('\\')[-1]
    quality = 0
    name_parts = var_name.split(',')
    for part in name_parts:
        if part[0] == 'm':
            model = part[2:]
            if have_pc and model not in model_list:
                raise ValueError(f"model {model} not available.") 
        if part[0] == 's':
            scenario = part[2:]
            if have_pc and scenario not in scenario_list:
                raise ValueError(f"scenario {scenario} not available.")
        if part[0] == 'v':
            variable = part[2:]
            if have_pc and variable not in variable_list:
                raise ValueError(f"variable {variable} not available.")
        if part[0] == 'q':
            quality = int(part[2:])
    if variable == None:
        raise ValueError('No variable name specified')
    return model, scenario, variable, quality

def _get_dataset(url):
    path = urlparse(url).path
    cache_entry = f'{cache_base}/{path}'
    if not os.path.exists(cache_entry):
        cache_dir = os.path.dirname(cache_entry)
        if not os.path.exists(cache_dir):
            os.makedirs(os.path.dirname(cache_entry))
        urlretrieve(url, filename=cache_entry)
    return(Dataset(cache_entry))

def _get_azure_url(url, var_name):
    ds = _get_dataset(url)
    return(ds[var_name])

def _get_cmip6_data(model, scenario, variable, start_date, end_date, lb, ub):
    result = None
    result_days = (end_date - start_date).days + 1
    if have_pc:
        search = catalog.search(
                collections=["nasa-nex-gddp-cmip6"],
                datetime=f'{start_date}/{end_date}',
                query = {
                    "cmip6:model": {
                        "eq": model
                    },
                    "cmip6:scenario": {
                        "in": ['historical', scenario]
                    },  
                },
                sortby=[{'field':'cmip6:year','direction':'asc'}]
        )
        items = search.item_collection()
            
    for item in items:
        if have_pc:
            year = item.properties['cmip6:year']
            url = item.assets[variable].href
            ds = _get_dataset(url)
        else:
            # TODO - in case indexing is offline, we still want to hit the cache
            pass
        data = ds[variable]
        if result is None:
            if lb[0] >= data[0].shape[0] or lb[1] >= data[0].shape[1]:
                return None
            ub = (min(ub[0]+1, data[0].shape[0]), min(ub[1]+1, data[0].shape[1]))
            shape = (result_days, ub[0] - lb[0], ub[1] - lb[1])
            result = np.ndarray(shape, dtype = data.dtype)
        item_start = max(start_date, date(year, 1,1))
        item_end = min(date(year,12,31), end_date)
        start_gidx = (item_start - start_date).days
        end_gidx = (item_end - start_date).days + 1
        start_iidx = (item_start - date(year, 1 , 1)).days
        end_iidx = (item_end - date(year, 1, 1)).days + 1
        result[start_gidx:end_gidx,:,:] = data[start_iidx:end_iidx,lb[0]:ub[0],lb[1]:ub[1]]
    return(result)

def reg_query(name, version, lb, ub, params, url, var_name):
    array = _get_azure_url(url, var_name)
    if lb:
        index = [ slice(lb[x], ub[x]+1) for x in range(len(lb)) ]
    else:
        index = [ slice(0, x) for x in array.shape]
    return(array[index])


def query(name, version, lb, ub):
    start_date, end_date = _get_gddp_time_ranges(version)
    model, scenario, variable, quality = _get_gddp_params(name)
    result = _get_cmip6_data(model, scenario, variable, start_date, end_date, lb, ub)
    return(result)

if __name__ == '__main__':
    s = date(2013, 5, 2)
    e = date(2013, 5, 2)
    start = (s - base_date).days
    span = (e - s).days
    lb = (0,0)
    ub = (599,1399)
    version = pack('uint:16, uint:16', start, span).uint
    res = query(name='cmip6-planetary\\m:ACCESS-ESM1-5,v:tas', version=1, lb=lb, ub=ub)
    print(res.shape)


