import planetary_computer 
from netCDF4 import Dataset 
import pystac_client 
import numpy as np 
import os 
from urllib.parse import urlparse 
from urllib.request import urlretrieve 
from bitstring import Bits, pack 
from datetime import date, timedelta 
from dateutil import parser 
import sys

base_date = date(1950, 1, 1)
present_date = date(2015, 1, 1)
last_date = date(2100, 12, 31)
cache_base = '.azrcache'

try: 
    catalog = pystac_client.Client.open("https://planetarycomputer.microsoft.com/api/stac/v1", 
                                        modifier = planetary_computer.sign_inplace, ) 
    collection = catalog.get_collection("nasa-nex-gddp-cmip6")
    variable_list = collection.summaries.get_list("cmip6:variable")
    model_list = collection.summaries.get_list("cmip6:model")[ : 10] 
    scenario_list = collection.summaries.get_list("cmip6:scenario")
    have_pc = True 
except pystac_client.exceptions.APIError:
    print("don't have planetary computer api access") 
    have_pc = False

                                                                                                                                                                                                                                                                                                                                                                                                                                                                          
def _get_gddp_time_ranges(version):
    bits = Bits(uint = version, length = 32) 
    start_day, span_days = bits.unpack('uint:16, uint:16')
    start_date = base_date + timedelta(days = start_day)
    end_date = start_date + timedelta(days = span_days) 
    if start_date > last_date:
        raise ValueError(f'WARNING: start_date of {start_date} is too far in the future') 
    if end_date > last_date:
        print(f'WARNING: truncating end_date of {end_date} to {last_date}')
        end_date = last_date 
    return start_date, end_date

def _validate_gddp_params(gparams):
    model = gparams.get('model', 'NO MODEL')
    scenario = gparams.get('scenario', 'NO SCENARIO')
    variable = gparams.get('variable', 'NO VARIABLE') 
    if model not in model_list:
        raise ValueError(f'model {model} not available.')
    if scenario not in scenario_list:
        raise ValueError(f'scenario {scenario} not available.')
    if variable not in variable_list:
        raise ValueError(f'variable {variable} not available.')

def _get_default_params(): 
    return {'model' : 'CESM2', 'scenario' : 'ssp585', 'variable' : None }

def _get_gddp_params(name):
    gddp_params = _get_default_params()
    var_name = name.split('\\')[- 1]
    name_parts = var_name.split(',')
    for part in name_parts: 
        if part[0] == 'm':
            gddp_params['model'] = part[2 : ] 
        if part[0] == 's':
            gddp_params['scenario'] = part[2 : ]
        if part[0] == 'v':
            gddp_params['variable'] = part[2 : ]
        if 'variable' not in gddp_params:
            raise ValueError('No variable name specified') 
    _validate_gddp_params(gddp_params)
    return gddp_params

def _get_dataset(url):
    path = urlparse(url).path
    cache_entry = f'{cache_base}/{path}'
    if not os.path.exists(cache_entry):
        cache_dir = os.path.dirname(cache_entry)
        if not os.path.exists(cache_dir):
            os.makedirs(os.path.dirname(cache_entry)) 
        urlretrieve(url, filename = cache_entry) 
    return Dataset(cache_entry)

def _get_azure_url(url, var_name):
    ds = _get_dataset(url) 
    return ds[var_name]

def _get_urls(start_date, end_date, model, scenario, variable):
    url_list = []
    search = catalog.search(collections = ["nasa-nex-gddp-cmip6"],
                            datetime = f'{start_date}/{end_date}', 
                            query = {"cmip6:model" : {"eq" : model }, 
                                     "cmip6:scenario" : {"in" : ['historical', scenario] }, }, 
                            sortby = [{'field' : 'cmip6:year', 'direction' : 'asc' }]) 
    items = search.item_collection()
    for item in items:
        year = item.properties['cmip6:year']
        url = item.assets[variable].href
        url_list.append((year, url)) 
    return url_list

def _get_cmip6_data(start_date, end_date, lb, ub, model, scenario, variable):
    result = None 
    result_days = (end_date - start_date).days + 1

    url_list = _get_urls(start_date, end_date, model, scenario, variable)

    for year, url in url_list:
        ds = _get_dataset(url) 
        data = ds[variable]
        if result is None: 
            if lb[0] >= data[0].shape[0] or lb[1] >= data[0].shape[1]: 
                return None 
            ub = (min(ub[0] + 1, data[0].shape[0]), min(ub[1] + 1, data[0].shape[1])) 
            shape = (result_days, ub[0] - lb[0], ub[1] - lb[1])
            result = np.ndarray(shape, dtype = data.dtype)
        item_start = max(start_date, date(year, 1, 1)) 
        item_end = min(date(year, 12, 31), end_date)
        start_gidx =(item_start - start_date).days
        end_gidx =(item_end - start_date).days + 1
        start_iidx =(item_start - date(year, 1, 1)).days
        end_iidx =(item_end - date(year, 1, 1)).days + 1 
        result[start_gidx:end_gidx, :, : ] = data[start_iidx:end_iidx, lb[0] :ub[0], lb[1] :ub[1]] 
        return result

def validate(start_date, end_date, variable, **kwargs):
    pass

def _get_reg_time_bounds(params, start_date, end_date):
    query_time = params.get('query_time')
    if query_time:
        query_start = parser.parse(query_time)
        query_end = parser.parse(query_time) 
    else:
        query_start = parser.parse(params.get('query_start'))
        query_end = parser.parse(params.get('query_end'))
    start_date = parser.parse(start_date)
    end_date = parser.parse(end_date)
    query_start = max(query_start, start_date)
    query_end = min(query_end, end_date)
    result_start = (query_start - start_date).days
    result_end =(query_end - start_date).days 
    
    return result_start, result_end

def _get_reg_params(variable, model, scenario):
    gddp_params = _get_default_params() 
    gddp_params['variable'] = variable 
    if model:
        gddp_params['model'] = model
    if scenario:
        gddp_params['scenario'] = scenario 
    return gddp_params

def _get_reg_data(gddp_params, start_date, end_date):
    url_list = _get_urls(start_date, end_date, 
                        gddp_params['model'],
                        gddp_params['scenario'], 
                        gddp_params['variable'])
    if len(url_list) != 1:
        raise ValueError('registry queries should involve exactly one file.') 
    _, url = url_list[0]
    ds = _get_dataset(url) 
    return ds[gddp_params['variable']]

def reg_query(name, params, variable, start_date, end_date, model = None, scenario = None):
    result_start, result_end = _get_reg_time_bounds(params, start_date, end_date)

    gddp_params = _get_reg_params(variable, model, scenario) 
    array = _get_reg_data(gddp_params, start_date, end_date)

    lb = params.get('lb') 
    ub = params.get('ub') 
    if lb:
        result = np.ndarray(((result_end - result_start) + 1, 
                            (ub[0] - lb[0]) + 1, (ub[1] - lb[1] + 1)), 
                            dtype = array.dtype)
        index = [slice(result_start, result_end + 1), 
                slice(lb[0], ub[0] + 1), 
                slice(lb[1], ub[1] + 1)]
    else:
        result = np.ndarray(((result_end - result_start) + 1, 
                            array.shape[0], array.shape[1]), 
                            dtype = array.dtype)
        index = [slice(result_start, result_end + 1), 
                slice(0, array.shape[0]), 
                slice(0, array.shape[1])] 
        result[ :, :, : ] = array[index]
    return result

def pquery(variable, start_date, end_date, lb, ub, model = 'CESM2', scenario = 'ssp585', **kwargs):
    result = _get_cmip6_data(model, scenario, variable, start_date, end_date, lb, ub)
    return result

def query(name, version, lb, ub):
    start_date, end_date = _get_gddp_time_ranges(version)
    gddp_params = _get_gddp_params(name)
    result = _get_cmip6_data(start_date, end_date, lb, ub, **gddp_params)
    return result

"""
if __name__ == '__main__':
    params = {'lb' : (100, 200), 
            'ub' : (150, 230), 
            'query_start' : '2013-05-02', 
            'query_end' : '2013-06-03'}
    #params = {'query_time' : '2013-05-02' }
    start_date = '2013-01-01'
    end_date = '2013-12-31'
    model = 'ACCESS-ESM1-5'
    scenario = 'historical'
    variable = 'tas'
    res = reg_query('foo', params, variable, start_date, end_date, model, scenario)
    print(res.shape)
"""

if __name__ == '__main__':
    s = date(2013, 5, 2) 
    e = date(2013, 5, 2)
    start =(s - base_date).days
    span = (e - s).days 
    lb =(100, 200) 
    ub =(150, 230)
    version = pack('uint:16, uint:16', start, span).uint
    res = query(name = 'cmip6-planetary\\m:ACCESS-ESM1-5,v:tas', version = 1, lb = lb, ub = ub)
    print(res)
