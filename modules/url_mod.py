from netCDF4 import Dataset
from urllib.parse import urlparse
from urllib.request import urlretrieve
import os

cache_base = '.url'

def _get_url_params(name):
    param_ps_list = name.split('\\')[-1]
    params_raw = param_ps_list.split('|')
    url = None
    var_name = None
    for p in params_raw:
        if p[0] == 'u':
            url = p[2:]
        if p[0] == 'v':
            var_name = p[2:]
    if not url:
        raise ValueError('no url found')
    return(url, var_name)

def _get_url_array(url, name, var_name):
    url_obj = urlparse(url)
    cache_entry = f'{cache_base}/{name}/{url_obj.path}'
    if not os.path.exists(cache_entry):
        cache_dir = os.path.dirname(cache_entry)
        os.makedirs(cache_dir, exist_ok=True)
        urlretrieve(url, cache_entry)
    data = Dataset(cache_entry)
    return(data[var_name])

def reg_query(name, params, url):
    if 'var_name' not in params:
        raise ValueError
    var_name = params['var_name']
    lb = params.get('lb')
    ub = params.get('ub')
    array = _get_url_array(url, name, var_name)
    if lb:
        index = [ slice(lb[x], ub[x]+1) for x in range(len(lb)) ]
    else:
        index = [ slice(0, x) for x in array.shape]
    return(array[index])

def validate(url, **kwargs):
    url_obj = urlparse(url)
    if not url_obj.path.endswith('.nc'):
        raise ValueError("URL module can only access netCDF files.")

def query(name, version, lb, ub):
    url, var_name = _get_url_params(name)
    validate(url)
    return(reg_query(None, version, lb, ub, {'var_name': var_name}, url))

if __name__ == '__main__':
    url = 'https://www.unidata.ucar.edu/software/netcdf/examples/sresa1b_ncar_ccsm3-example.nc'
    params = {'lb':(10,10), 'ub':(20,20), 'var_name':'area'}
    a = reg_query('foo', params, url)
    print(a)