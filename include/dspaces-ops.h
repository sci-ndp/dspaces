#ifndef __DSPACES_OP_H__
#define __DSPACES_OP_H__

#include<dspaces.h>
#include<ss_data.h>

typedef enum ds_operator {
    DS_OP_OBJ,
    DS_OP_ICONST,
    DS_OP_RCONST,
    DS_OP_ADD,
    DS_OP_SUB,
    DS_OP_MULT,
    DS_OP_DIV,
    DS_OP_POW
} ds_op_t;

typedef enum ds_val_type {
    DS_VAL_INT,
    DS_VAL_REAL
} ds_val_t;

typedef struct ds_data_expr {
    ds_op_t op;
    union {
        long ival;
        double rval;
        struct obj_data *od;
    };
    ds_val_t type; 
    struct ds_data_expr **sub_expr;
    uint64_t size;
} *ds_expr_t;

static inline hg_return_t hg_proc_ds_expr_t(hg_proc_t proc, void *data)
{
    void **datav;
    hg_return_t ret;
    int8_t byte;
    int i;

    ds_expr_t buf = *((ds_expr_t *)data);
    switch(hg_proc_get_op(proc)) {
    case HG_ENCODE:
        byte = buf->op;
        hg_proc_uint8_t(proc, &byte);
        byte = buf->type;
        hg_proc_uint8_t(proc, &byte);
        hg_proc_uint64_t(proc, &buf->size);
        switch(buf->op) {
            case DS_OP_OBJ:
                hg_proc_raw(proc, buf->od, sizeof(*buf->od));
                ret = HG_SUCCESS;
                break;
            case DS_OP_ICONST:
                hg_proc_int32_t(proc, &buf->ival);
                ret = HG_SUCCESS;
                break;
            case DS_OP_RCONST:
                hg_proc_int64_t(proc, &buf->rval);
                ret = HG_SUCCESS;
                break;
            case DS_OP_ADD:
            case DS_OP_SUB:
            case DS_OP_MULT:
            case DS_OP_DIV:
            case DS_OP_POW:
                byte = 2;
                hg_proc_uint8_t(proc, &byte);
                hg_proc_ds_expr_t(proc, &buf->sub_expr[0]);
                hg_proc_ds_expr_t(proc, &buf->sub_expr[1]);
                ret = HG_SUCCESS;
                break;
            default:
                ret = HG_INVALID_PARAM;
        }
        break;
    case HG_DECODE:
        datav = data;
        buf = malloc(sizeof(struct ds_data_expr));
        *datav = buf;
        hg_proc_uint8_t(proc, &byte);
        buf->op = byte;
        hg_proc_uint8_t(proc, &byte);
        buf->type = byte;
        hg_proc_uint64_t(proc, &buf->size);
        switch(buf->op) {
            case DS_OP_OBJ:
                buf->od = malloc(sizeof(*buf->od));
                hg_proc_raw(proc, buf->od, sizeof(*buf->od));
                ret = HG_SUCCESS;
                break;
            case DS_OP_ICONST:
                hg_proc_int32_t(proc, &buf->ival);
                ret = HG_SUCCESS;
                break;
            case DS_OP_RCONST:
                hg_proc_int64_t(proc, &buf->rval);
                ret = HG_SUCCESS;
                break;
            case DS_OP_ADD:
            case DS_OP_SUB:
            case DS_OP_MULT:
            case DS_OP_DIV:
            case DS_OP_POW:
                hg_proc_uint8_t(proc, &byte);
                buf->sub_expr = malloc(byte * sizeof(*buf->sub_expr));
                for(i = 0; i < byte; i++) {
                    //buf->sub_expr[i] = malloc(sizeof(**buf->sub_expr));
                    hg_proc_ds_expr_t(proc, &buf->sub_expr[i]);
                }
                ret = HG_SUCCESS;
                break;
            default:
                ret = HG_PROTOCOL_ERROR;
        }
        break;
    case HG_FREE:
        switch(buf->op) {
            case DS_OP_OBJ:
                free(buf->od);
                ret = HG_SUCCESS;
                break;
            case DS_OP_ICONST:
            case DS_OP_RCONST:
                ret = HG_SUCCESS;
                break;
            case DS_OP_ADD:
            case DS_OP_SUB:
            case DS_OP_MULT:
            case DS_OP_DIV:
            case DS_OP_POW:
                free(buf->sub_expr[0]);
                free(buf->sub_expr[1]);
                free(buf->sub_expr);
                ret = HG_SUCCESS;
                break;
             default:
                ret = HG_INVALID_PARAM;
        }
        break;
    }

    return(ret);
}

MERCURY_GEN_PROC(do_ops_in_t, ((hg_bulk_t)(handle))((ds_expr_t)(expr)))

struct ds_data_expr *dspaces_op_new_obj(dspaces_client_t client, const char *var_name, unsigned int ver, ds_val_t type, int ndim, uint64_t *lb, uint64_t *ub);

struct ds_data_expr *dspaces_op_new_iconst(long val);

struct ds_data_expr *dspaces_op_new_rconst(double val);

struct ds_data_expr *dspaces_op_new_add(struct ds_data_expr *expr1, struct ds_data_expr *expr2);

struct ds_data_expr *dspaces_op_new_sub(struct ds_data_expr *expr1, struct ds_data_expr *expr2);

struct ds_data_expr *dspaces_op_new_mult(struct ds_data_expr *expr1, struct ds_data_expr *expr2);

struct ds_data_expr *dspaces_op_new_div(struct ds_data_expr *expr1, struct ds_data_expr *expr2);

struct ds_data_expr *dspaces_op_new_pow(struct ds_data_expr *expr1, struct ds_data_expr *expr2);

struct ds_data_expr *dspaces_op_new_2arg(ds_op_t op, struct ds_data_expr *expr1, struct ds_data_expr *expr2);

double ds_op_calc_rval(struct ds_data_expr *expr, long pos, int *res);

long ds_op_calc_ival(struct ds_data_expr *expr, long pos, int *res);

void gather_op_ods(struct ds_data_expr *expr, struct list_head *expr_odl);

void update_expr_objs(struct ds_data_expr *expr, struct obj_data *od);

#endif /* __DSPACES_OP_H__ */
