import sys
import json
import importlib
import sqlite3

class Registry:
    def __init__(self, db_name = 'dspaces_reg.db'):
        self.db_name = db_name
        db_conn = sqlite3.connect(db_name)
        cursor = db_conn.cursor()
        cursor.execute("CREATE TABLE IF NOT EXISTS type (id INTEGER PRIMARY KEY, name TEXT(256), module TEXT(256))")
        cursor.execute("CREATE TABLE IF NOT EXISTS registry (id INTEGER PRIMARY KEY, reg_type INTEGER, name TEXT(256), data BLOCK, FOREIGN KEY(reg_type) REFERENCES type(id), UNIQUE(reg_type, name))")
        self.modules = {}
        for (name, module) in cursor.execute("SELECT name, module FROM type"):
            self.modules[name] = importlib.import_module(module)
        db_conn.commit()
        cursor.close()

    def Add(self, reg_id, reg_type, name, data):
        db_conn = sqlite3.connect(self.db_name)
        cursor = db_conn.cursor()
        if reg_type not in self.modules:
            cursor.execute("INSERT INTO type(name, module) VALUES(?, ?)", (reg_type, reg_type))
            type_id = cursor.lastrowid
            self.modules[reg_type] = importlib.import_module(reg_type)
        else:
            res = cursor.execute("SELECT id FROM type WHERE name == ?", (reg_type,))
            type_id, = res.fetchone()
        self.modules[reg_type].validate(**json.loads(data))
        res = cursor.execute("SELECT id FROM registry WHERE reg_type == ? AND name == ?", (type_id, name)).fetchone()
        if res is None:
            cursor.execute("INSERT INTO registry(id, reg_type, name, data) VALUES(?, ?, ?, ?) ON CONFLICT DO NOTHING", (reg_id, type_id, name, data))
        else:
            reg_id, = res
        db_conn.commit()
        cursor.close()
        return(reg_id)

    def Query(self, reg_id, version, lb, ub, params):
        db_conn = sqlite3.connect(self.db_name)
        cursor = db_conn.cursor()
        res = cursor.execute("SELECT type.name, registry.name, registry.data FROM registry INNER JOIN type ON registry.reg_type == type.id WHERE registry.id == ?", (reg_id,))
        (reg_type, name, ser_data) = res.fetchone()
        db_conn.close()
        data = json.loads(ser_data)
        module = self.modules[reg_type]
        return(module.reg_query(name, version, lb, ub, params, **data))

    def HighestIDBetween(self, min_id, max_id):
        db_conn = sqlite3.connect(self.db_name)
        cursor = db_conn.cursor()
        res = cursor.execute("SELECT MAX(id) FROM registry WHERE id >= ? AND id <= ?", (min_id, max_id))
        reg_id, = res.fetchone()
        if reg_id is None:
            reg_id = 0
        return(reg_id)

reg = Registry()

def register(type, name, data, id):
    return(reg.Add(id, type, name, data))

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

def bootstrap_id(rank):
    min_id = rank << 40
    max_id = ((rank + 1) << 40) - 1
    reg_id = reg.HighestIDBetween(min_id, max_id)
    return(reg_id + 1)

def query(name, version, lb, ub):
    id_part = name.split('\\')[-1]
    id, params = _get_query_params(name)
    return(reg.Query(id, version, lb, ub, params))

if __name__ == '__main__':
    register('s3nc_mod', 'abc', json.dumps({'bucket':'noaa-goes17','path':'ABI-L1b-RadM/2020/215/15/OR_ABI-L1b-RadM1-M6C02_G17_s20202151508255_e20202151508312_c20202151508338.nc'}), 45)
    res = query('abcdef\id:45,var_name:Rad', 2, (1,1), (4,2))
    print(res)
    
