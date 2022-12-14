#ifndef __DSPACES_OP_H__
#define __DSPACES_OP_H__

#include<ss_data.h>

typedef enum ds_operator {
    DS_OP_OBJ,
    DS_OP_ICONST,
    DS_OP_RCONST,
    DS_OP_ADD
} ds_op_t;

typedef enum ds_val_type {
    DS_VAL_INT,
    DS_VAL_REAL
} ds_val_t;

struct ds_data_expr {
    ds_op_t op;
    union {
        int ival;
        double rval;
        obj_descriptor *odsc;        
        struct obj_data *od;
    };
    ds_val_t type; 
    struct ds_data_expr **sub_expr;
};

struct ds_data_expr *dspaces_op_new_obj(const char *var_name, unsigned int ver, ds_val_t type, int ndim, uint64_t *lb, uint64_t *ub);

struct ds_data_expr *dspaces_op_new_iconst(int val);

struct ds_data_expr *dspaces_op_new_rconst(double val);

struct ds_data_expr *dspaces_op_new_add(struct ds_data_expr *expr1, struct ds_data_expr *expr2);

struct ds_data_expr *dspaces_op_new_2arg(ds_op_t op, struct ds_data_expr *expr1, struct ds_data_expr *expr2);

double ds_op_calc_rval(struct ds_data_expr *expr, int pos, int *res);

int ds_op_calc_ival(struct ds_data_expr *expr, int pos, int *res);

#endif /* __DSPACES_OP_H__ */
