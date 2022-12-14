#include<list.h>
#include<dspaces-ops.h>

struct ds_data_expr *dspaces_op_new_obj(const char *var_name, unsigned int ver, ds_val_t type, int ndim, uint64_t *lb, uint64_t *ub)
{
    struct ds_data_expr *expr = malloc(sizeof(*expr));
    obj_descriptor *odsc = malloc(sizeof(*odsc));

    odsc->version = ver;
    if(type == DS_VAL_INT) {
        odsc->size = sizeof(int);
    } else if(type == DS_VAL_REAL) {
        odsc->size = sizeof(double);
    } else {
        fprintf(stderr, "ERROR: %s: unknown type.\n", __func__);
        free(expr);
        free(odsc);
        return(NULL);
    }
    odsc->bb.num_dims = ndim;

    memset(odsc->bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(odsc->bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(odsc->bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(odsc->bb.ub.c, ub, sizeof(uint64_t) * ndim);

    strncpy(odsc->name, var_name, sizeof(odsc->name) - 1);
    odsc->name[sizeof(odsc->name) - 1] = '\0';
   
    expr->op = DS_OP_OBJ;
    expr->odsc = odsc;
    expr->type = type;
    expr->sub_expr = NULL;

    return(expr);
}

struct ds_data_expr *dspaces_op_new_iconst(int val)
{
    struct ds_data_expr *expr = malloc(sizeof(*expr));

    expr->op = DS_OP_ICONST;
    expr->ival = val;
    expr->type = DS_VAL_INT;
    expr->sub_expr = NULL;
}

struct ds_data_expr *dspaces_op_new_rconst(double val)
{
    struct ds_data_expr *expr = malloc(sizeof(*expr));

    expr->op = DS_OP_RCONST;
    expr->rval = val;
    expr->type = DS_VAL_REAL;
    expr->sub_expr = NULL;
}

struct ds_data_expr *dspaces_op_new_add(struct ds_data_expr *expr1, struct ds_data_expr *expr2)
{
    return(dspaces_op_new_2arg(DS_OP_ADD, expr1, expr2));
}

struct ds_data_expr *dspaces_op_new_2arg(ds_op_t op, struct ds_data_expr *expr1, struct ds_data_expr *expr2)
{
    struct ds_data_expr *expr;
    
    if(op != DS_OP_ADD) {
        fprintf(stderr, "ERROR: %s: unkown opcode %i.\n", __func__, op);
        return(NULL);
    }

    expr = malloc(sizeof(*expr));
    expr->op = op;

    expr->type = DS_VAL_INT;
    if(expr1->type == DS_VAL_REAL || expr2->type == DS_VAL_REAL) {
        expr->type = DS_VAL_REAL;
    }

    expr->sub_expr = malloc(2 * sizeof(*expr->sub_expr));
    expr->sub_expr[0] = expr1;
    expr->sub_expr[1] = expr2;

    return(expr);
}



double ds_op_calc_rval(struct ds_data_expr *expr, int pos, int *res)
{
    struct obj_data *od;
    double subval1, subval2;
    int err = 0;
    *res = 0;

    if(expr->type != DS_VAL_REAL) {
        *res = -1;
        fprintf(stderr, "ERROR: %s: attempted to calculate a real result of a non-real expression.\n", __func__);
        return(0);
    }

    switch(expr->type) {
        case DS_OP_OBJ:
            od = expr->od;
            return(((double *)od->data)[pos]);
        case DS_OP_ICONST:
            *res = -1;
            fprintf(stderr, "ERROR: %s: attempted illegal cast of int to real (corruption?)\n", __func__);
            return(0);
        case DS_OP_RCONST:
            return(expr->rval);
        case DS_OP_ADD:
            if(expr->sub_expr[0]->type == DS_VAL_REAL) {
                subval1 = ds_op_calc_rval(expr->sub_expr[0], pos, &err);
            } else if(expr->sub_expr[0]->type == DS_VAL_INT) {
                subval1 = ds_op_calc_ival(expr->sub_expr[0], pos, &err);
            }
            if(err < 0) {
                *res = err;
                return(0);
            }
            if(expr->sub_expr[1]->type == DS_VAL_REAL) {
                subval2 = ds_op_calc_rval(expr->sub_expr[1], pos, &err);
            } else if(expr->sub_expr[1]->type == DS_VAL_INT) {
                subval2 = ds_op_calc_ival(expr->sub_expr[1], pos, &err);
            }
            if(err < 0) {
                *res = err;
                return(0);
            }
            break;
        default:
            *res = -1;
            fprintf(stderr, "ERROR: %s: unknown opcode %i\n", __func__, expr->type);
            return(0);
    }

    switch(expr->type) {
        case DS_OP_ADD:
            return(subval1 + subval2);
        default:
            fprintf(stderr, "ERROR: %s: no way to handle unknown op %i (corruption?)\n", __func__, expr->type);
            *res = -1;
            return(0);
    }

    return(0); 
}

int ds_op_calc_ival(struct ds_data_expr *expr, int pos, int *res)
{
    struct obj_data *od;
    int subval1, subval2;
    int err = 0;
    *res = 0;

    if(expr->type != DS_VAL_REAL) {
        *res = -1;
        fprintf(stderr, "ERROR: %s: attempted to calculate a real result of a non-real expression.\n", __func__);
        return(0);
    }

    switch(expr->type) {
        case DS_OP_OBJ:
            od = expr->od;
            return(((double *)od->data)[pos]);
        case DS_OP_RCONST:
            *res = -1;
            fprintf(stderr, "ERROR: %s: attempted illegal cast of int to real (corruption?)\n", __func__);
            return(0);
        case DS_OP_ICONST:
            return(expr->ival);
        case DS_OP_ADD:
            if(expr->sub_expr[0]->type == DS_VAL_REAL) {
                subval1 = ds_op_calc_rval(expr->sub_expr[0], pos, &err);
            } else if(expr->sub_expr[0]->type == DS_VAL_INT) {
                subval1 = ds_op_calc_ival(expr->sub_expr[0], pos, &err);
            }
            if(err < 0) {
                *res = err;
                return(0);
            }
            if(expr->sub_expr[1]->type == DS_VAL_REAL) {
                subval2 = ds_op_calc_rval(expr->sub_expr[1], pos, &err);
            } else if(expr->sub_expr[1]->type == DS_VAL_INT) {
                subval2 = ds_op_calc_ival(expr->sub_expr[1], pos, &err);
            }
            if(err < 0) {
                *res = err;
                return(0);
            }
            break;
        default:
            *res = -1;
            fprintf(stderr, "ERROR: %s: unknown opcode %i\n", __func__, expr->type);
            return(0);
    }

    switch(expr->type) {
        case DS_OP_ADD:
            return(subval1 + subval2);
        default:
            fprintf(stderr, "ERROR: %s: no way to handle unknown op %i (corruption?)\n", __func__, expr->type);
            *res = -1;
            return(0);
    }

    return(0); 
}

void gather_op_odscs(struct ds_data_expr *expr, struct list_head *expr_odscl)
{
    obj_descriptor *odsc, *expr_odsc;
    struct obj_desc_ptr_list *odscl;
    int found = 0;

    switch(expr->type) {
        case DS_OP_OBJ:    
            expr_odsc = expr->odsc;
            list_for_each_entry(odscl, expr_odscl, struct obj_desc_ptr_list, odsc_entry) {
                odsc = odscl->odsc;
                if(strcmp(odsc->name, expr_odsc->name) == 0) {
                    found = 1;
                    break;
                }
            }
            if(found) {
                odscl = malloc(sizeof(*odscl));
                odscl->odsc = expr_odsc;
                list_add(&odscl->odsc_entry, expr_odscl);  
            }
            break;
        case DS_OP_ADD:
            gather_op_odscs(expr->sub_expr[0], expr_odscl);
            gather_op_odscs(expr->sub_expr[1], expr_odscl);
            break;
        default:
            break;
    }
}

void dspaces_op_rpc()
{
    struct obj_desc_ptr_list *odsc_ptr;
    struct obj_data *od, *od_expr;
    struct list_head odscl, odl;
    struct ds_data_expr *expr;
    obj_descriptor *odsc;

    INIT_LIST_HEAD(&odscl);
    gather_op_odscs(expr, &odscl);
    INIT_LIST_HEAD(&odl);
    list_for_each_entry(odsc_ptr, &odscl, struct obj_desc_ptr_list, odsc_entry) {
        odsc = odsc_ptr->odsc;
        //od = ls_find(server->dcg->ls, odsc);
        if(od) {
            od_expr = obj_data_alloc(odsc);
            ssd_copy(od_expr, od);
        } else {
            //od_expr = obj_data_alloc_with_data(odsc, void *);
            //query remotes
        }
        list_add(&od_expr->obj_entry, &odl);
    }

    map_odscs_to_ods(expr, &odl);    
}
