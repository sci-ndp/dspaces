import sys
import json
import importlib

class RegEntry:
    def __init__(self, type, name, data):
        module = importlib.import_module(type)
        self.query = module.reg_query
        self.name = name
        self.data = data

    def Query(self, version, lb, ub, params):
        return(self.query(self.name, version, lb, ub, params, **self.data))

reg_db = {}

def register(type, name, data, id):
    reg_db[id] = RegEntry(type, name, json.loads(data)) 

def _get_query_params(name):
    param_part = name.split('\\')[-1]
    parts = param_part.split(',')
    params = {}
    for part in parts:
        key, val = part.split(':',1 )
        if key == 'id':
            id = val
        else:
            params[key] = val
    return int(id), params

def query(name, version, lb, ub):
    id_part = name.split('\\')[-1]
    id, params = _get_query_params(name)
    reg = reg_db[id]
    return(reg.Query(version, lb, ub, params))

if __name__ == '__main__':
    register('s3nc_mod', 'abc', json.dumps({'bucket':'noaa-goes17','path':'ABI-L1b-RadM/2020/215/15/OR_ABI-L1b-RadM1-M6C02_G17_s20202151508255_e20202151508312_c20202151508338.nc'}), 45)
    res = query('abcdef\id:45,var_name:Rad', 2, (1,1), (4,2))
    print(res)
    
