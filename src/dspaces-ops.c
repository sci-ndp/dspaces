#include <bbox.h>
#include <dspaces-ops.h>
#include <dspaces.h>
#include <list.h>
#include <math.h>
#include <ss_data.h>

struct ds_data_expr *dspaces_op_new_obj(dspaces_client_t client,
                                        const char *var_name, unsigned int ver,
                                        ds_val_t type, int ndim, uint64_t *lb,
                                        uint64_t *ub)
{
    struct ds_data_expr *expr = malloc(sizeof(*expr));
    struct obj_data *od = malloc(sizeof(*od));
    int gndim;
    uint64_t gdim[BBOX_MAX_NDIM];

    od->obj_desc.version = ver;
    if(type == DS_VAL_INT) {
        od->obj_desc.size = sizeof(int);
    } else if(type == DS_VAL_REAL) {
        od->obj_desc.size = sizeof(double);
    } else {
        fprintf(stderr, "ERROR: %s: unknown type.\n", __func__);
        goto err_free;
    }
    od->obj_desc.bb.num_dims = ndim;

    memset(od->obj_desc.bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(od->obj_desc.bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(od->obj_desc.bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(od->obj_desc.bb.ub.c, ub, sizeof(uint64_t) * ndim);

    strncpy(od->obj_desc.name, var_name, sizeof(od->obj_desc.name) - 1);
    od->obj_desc.name[sizeof(od->obj_desc.name) - 1] = '\0';

    /* This is an encapsulation violation and should be fixed. The problem is
     * that the client handle encapsualtes the default gdim structure, and the
     * gdim type is not exposed to the client.
     * */
    dspaces_get_gdim(client, var_name, &od->gdim.ndim, od->gdim.sizes.c);
    if(od->gdim.ndim != ndim) {
        fprintf(
            stderr,
            "ERROR: %s: previously declared gdims different dimensionality!\n",
            __func__);
        goto err_free;
    }

    expr->op = DS_OP_OBJ;
    expr->od = od;
    expr->type = type;
    expr->size = od->obj_desc.size * bbox_volume(&od->obj_desc.bb);
    expr->sub_expr = NULL;

    return (expr);
err_free:
    free(expr);
    free(od);
    return (NULL);
}

struct ds_data_expr *dspaces_op_new_iconst(long val)
{
    struct ds_data_expr *expr = malloc(sizeof(*expr));

    expr->op = DS_OP_ICONST;
    expr->ival = val;
    expr->type = DS_VAL_INT;
    expr->size = sizeof(int);
    expr->sub_expr = NULL;

    return(expr);
}

struct ds_data_expr *dspaces_op_new_rconst(double val)
{
    struct ds_data_expr *expr = malloc(sizeof(*expr));

    expr->op = DS_OP_RCONST;
    expr->rval = val;
    expr->type = DS_VAL_REAL;
    expr->size = sizeof(double);
    expr->sub_expr = NULL;

    return(NULL);
}

struct ds_data_expr *dspaces_op_new_add(struct ds_data_expr *expr1,
                                        struct ds_data_expr *expr2)
{
    return (dspaces_op_new_2arg(DS_OP_ADD, expr1, expr2));
}

struct ds_data_expr *dspaces_op_new_sub(struct ds_data_expr *expr1,
                                        struct ds_data_expr *expr2)
{
    return (dspaces_op_new_2arg(DS_OP_SUB, expr1, expr2));
}

struct ds_data_expr *dspaces_op_new_mult(struct ds_data_expr *expr1,
                                         struct ds_data_expr *expr2)
{
    return (dspaces_op_new_2arg(DS_OP_MULT, expr1, expr2));
}

struct ds_data_expr *dspaces_op_new_div(struct ds_data_expr *expr1,
                                        struct ds_data_expr *expr2)
{
    return (dspaces_op_new_2arg(DS_OP_DIV, expr1, expr2));
}

struct ds_data_expr *dspaces_op_new_pow(struct ds_data_expr *expr1,
                                        struct ds_data_expr *expr2)
{
    return (dspaces_op_new_2arg(DS_OP_POW, expr1, expr2));
}

struct ds_data_expr *dspaces_op_new_arctan(struct ds_data_expr *expr)
{
    return (dspaces_op_new_1arg(DS_OP_ARCTAN, expr));
}

struct ds_data_expr *dspaces_op_new_1arg(ds_op_t op, struct ds_data_expr *expr1)
{
    struct ds_data_expr *expr;

    if(op != DS_OP_ARCTAN) {
        fprintf(stderr, "ERROR: %s: unknown unary opcode %i.\n", __func__, op);
    }

    expr = malloc(sizeof(*expr));
    expr->op = op;
    if(op == DS_OP_ARCTAN || op == DS_OP_DIV) {
        expr->type = DS_VAL_REAL;
    } else {
        expr->type = expr1->type;
    }
    expr->size = expr1->size;
    expr->sub_expr = malloc(sizeof(*expr->sub_expr));
    expr->sub_expr[0] = expr1;

    return (expr);
}

struct ds_data_expr *dspaces_op_new_2arg(ds_op_t op, struct ds_data_expr *expr1,
                                         struct ds_data_expr *expr2)
{
    struct ds_data_expr *expr;

    if(op < DS_OP_ADD || op > DS_OP_POW) {
        fprintf(stderr, "ERROR: %s: unknown binary opcode %i.\n", __func__, op);
        return (NULL);
    }

    expr = malloc(sizeof(*expr));
    expr->op = op;

    expr->type = DS_VAL_INT;
    if(expr1->type == DS_VAL_REAL || expr2->type == DS_VAL_REAL) {
        expr->type = DS_VAL_REAL;
    }

    expr->size = expr1->size > expr2->size ? expr1->size : expr2->size;

    expr->sub_expr = malloc(2 * sizeof(*expr->sub_expr));
    expr->sub_expr[0] = expr1;
    expr->sub_expr[1] = expr2;

    return (expr);
}

double ds_op_calc_rval(struct ds_data_expr *expr, long pos, int *res)
{
    struct obj_data *od;
    double subval1, subval2;
    int err = 0;
    *res = 0;

    if(expr->type != DS_VAL_REAL) {
        *res = -1;
        fprintf(stderr,
                "ERROR: %s: attempted to calculate a real result of a non-real "
                "expression.\n",
                __func__);
        return (0);
    }

    switch(expr->op) {
    case DS_OP_OBJ:
        od = expr->od;
        return (((double *)od->data)[pos]);
    case DS_OP_ICONST:
        *res = -1;
        fprintf(
            stderr,
            "ERROR: %s: attempted illegal cast of int to real (corruption?)\n",
            __func__);
        return (0);
    case DS_OP_RCONST:
        return (expr->rval);
    case DS_OP_ADD:
    case DS_OP_SUB:
    case DS_OP_MULT:
    case DS_OP_DIV:
    case DS_OP_POW:
        if(expr->sub_expr[0]->type == DS_VAL_REAL) {
            subval1 = ds_op_calc_rval(expr->sub_expr[0], pos, &err);
        } else if(expr->sub_expr[0]->type == DS_VAL_INT) {
            subval1 = ds_op_calc_ival(expr->sub_expr[0], pos, &err);
        }
        if(err < 0) {
            *res = err;
            return (0);
        }
        if(expr->sub_expr[1]->type == DS_VAL_REAL) {
            subval2 = ds_op_calc_rval(expr->sub_expr[1], pos, &err);
        } else if(expr->sub_expr[1]->type == DS_VAL_INT) {
            subval2 = ds_op_calc_ival(expr->sub_expr[1], pos, &err);
        }
        if(err < 0) {
            *res = err;
            return (0);
        }
        break;
    case DS_OP_ARCTAN:
        if(expr->sub_expr[0]->type == DS_VAL_REAL) {
            subval1 = ds_op_calc_rval(expr->sub_expr[0], pos, &err);
        } else if(expr->sub_expr[0]->type == DS_VAL_INT) {
            subval1 = ds_op_calc_ival(expr->sub_expr[0], pos, &err);
        }
        if(err < 0) {
            *res = err;
            return (0);
        }
        break;
    default:
        *res = -1;
        fprintf(stderr, "ERROR: %s: unknown opcode %i\n", __func__, expr->type);
        return (0);
    }

    switch(expr->op) {
    case DS_OP_ADD:
        return (subval1 + subval2);
    case DS_OP_SUB:
        return (subval1 - subval2);
    case DS_OP_MULT:
        return (subval1 * subval2);
    case DS_OP_DIV:
        return (subval1 / subval2);
    case DS_OP_POW:
        return (pow(subval1, subval2));
    case DS_OP_ARCTAN:
        return (atan(subval1));
    default:
        fprintf(stderr,
                "ERROR: %s: no way to handle unknown op %i (corruption?)\n",
                __func__, expr->type);
        *res = -1;
        return (0);
    }

    return (0);
}

long ds_op_calc_ival(struct ds_data_expr *expr, long pos, int *res)
{
    struct obj_data *od;
    long subval1, subval2;
    int err = 0;
    *res = 0;

    if(expr->type != DS_VAL_INT) {
        *res = -1;
        fprintf(stderr,
                "ERROR: %s: attempted to calculate an integer result of a "
                "non-integer "
                "expression.\n",
                __func__);
        return (0);
    }

    switch(expr->op) {
    case DS_OP_OBJ:
        od = expr->od;
        return (((double *)od->data)[pos]);
    case DS_OP_RCONST:
        *res = -1;
        fprintf(
            stderr,
            "ERROR: %s: attempted illegal cast of real to int (corruption?)\n",
            __func__);
        return (0);
    case DS_OP_ICONST:
        return (expr->ival);
    case DS_OP_ADD:
    case DS_OP_SUB:
    case DS_OP_MULT:
    case DS_OP_DIV:
    case DS_OP_POW:
        if(expr->sub_expr[0]->type == DS_VAL_REAL) {
            subval1 = ds_op_calc_rval(expr->sub_expr[0], pos, &err);
        } else if(expr->sub_expr[0]->type == DS_VAL_INT) {
            subval1 = ds_op_calc_ival(expr->sub_expr[0], pos, &err);
        }
        if(err < 0) {
            *res = err;
            return (0);
        }
        if(expr->sub_expr[1]->type == DS_VAL_REAL) {
            subval2 = ds_op_calc_rval(expr->sub_expr[1], pos, &err);
        } else if(expr->sub_expr[1]->type == DS_VAL_INT) {
            subval2 = ds_op_calc_ival(expr->sub_expr[1], pos, &err);
        }
        if(err < 0) {
            *res = err;
            return (0);
        }
        break;
    default:
        *res = -1;
        fprintf(stderr, "ERROR: %s: unknown opcode %i\n", __func__, expr->type);
        return (0);
    }

    // Do this as a subsequent switch so we can group all the n-args together
    // above
    switch(expr->op) {
    case DS_OP_ADD:
        return (subval1 + subval2);
    case DS_OP_SUB:
        return (subval1 - subval2);
    case DS_OP_MULT:
        return (subval1 * subval2);
    case DS_OP_DIV:
        return (subval1 / subval2);
    case DS_OP_POW:
        return (pow(subval1, subval2));

    default:
        fprintf(stderr,
                "ERROR: %s: no way to handle unknown op %i (corruption?)\n",
                __func__, expr->type);
        *res = -1;
        return (0);
    }

    return (0);
}

void gather_op_ods(struct ds_data_expr *expr, struct list_head *expr_odl)
{
    struct obj_data *od, *expr_od;
    int found = 0;

    switch(expr->op) {
    case DS_OP_OBJ:
        expr_od = expr->od;
        list_for_each_entry(od, expr_odl, struct obj_data, obj_entry)
        {
            if(strcmp(od->obj_desc.name, expr_od->obj_desc.name) == 0) {
                found = 1;
                break;
            }
        }
        if(!found) {
            list_add(&expr_od->obj_entry, expr_odl);
        }
        break;
    case DS_OP_ADD:
    case DS_OP_SUB:
    case DS_OP_MULT:
    case DS_OP_DIV:
    case DS_OP_POW:
        gather_op_ods(expr->sub_expr[0], expr_odl);
        gather_op_ods(expr->sub_expr[1], expr_odl);
        break;
    case DS_OP_ARCTAN:
        gather_op_ods(expr->sub_expr[0], expr_odl);
        break;
    default:
        break;
    }
}

void update_expr_objs(struct ds_data_expr *expr, struct obj_data *od)
{
    const char *od_name = od->obj_desc.name;
    char *expr_obj_name;

    switch(expr->op) {
    case DS_OP_OBJ:
        expr_obj_name = expr->od->obj_desc.name;
        if(strcmp(od_name, expr_obj_name) == 0) {
            expr->od = od;
        }
        break;
    case DS_OP_ADD:
    case DS_OP_SUB:
    case DS_OP_MULT:
    case DS_OP_DIV:
    case DS_OP_POW:
        update_expr_objs(expr->sub_expr[0], od);
        update_expr_objs(expr->sub_expr[1], od);
        break;
    case DS_OP_ARCTAN:
        update_expr_objs(expr->sub_expr[0], od);
        break;
    default:
        break;
    }
}

int dspaces_op_get_result_type(struct ds_data_expr *expr)
{
    return (expr->type);
}

void dspaces_op_get_result_size(struct ds_data_expr *expr, int *ndim,
                                uint64_t **dims)
{
    int lndim, rndim;
    uint64_t *ldims, *rdims;
    int i;

    switch(expr->op) {
    case DS_OP_OBJ:
        *ndim = expr->od->obj_desc.bb.num_dims;
        *dims = malloc(*ndim * sizeof(**dims));
        for(i = 0; i < *ndim; i++) {
            (*dims)[i] = (expr->od->obj_desc.bb.ub.c[i] -
                          expr->od->obj_desc.bb.lb.c[i]) +
                         1;
        }
        break;
    case DS_OP_ICONST:
    case DS_OP_RCONST:
        *ndim = 0;
        *dims = NULL;
        break;
    case DS_OP_ADD:
    case DS_OP_SUB:
    case DS_OP_MULT:
    case DS_OP_DIV:
    case DS_OP_POW:
        dspaces_op_get_result_size(expr->sub_expr[0], &lndim, &ldims);
        dspaces_op_get_result_size(expr->sub_expr[1], &rndim, &rdims);
        if(lndim > rndim) {
            *ndim = lndim;
            *dims = ldims;
            free(rdims);
        } else {
            *ndim = rndim;
            *dims = rdims;
            free(ldims);
        }
        break;
    case DS_OP_ARCTAN:
        dspaces_op_get_result_size(expr->sub_expr[0], &lndim, &ldims);
        *ndim = lndim;
        *dims = ldims;
        break;
    default:
        fprintf(stderr, "ERROR: %s: unknown op type.\n", __func__);
    }
}
