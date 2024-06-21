#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

typedef int fixed_point;

fixed_point fxp_of_int(int q, int);
int fxp_trunc(int q, fixed_point);
int fxp_round(int q, fixed_point);
fixed_point fxp_reformat(int old_q, int new_q, fixed_point);

fixed_point fxp_add(fixed_point, fixed_point);
fixed_point fxp_sub(fixed_point, fixed_point);
fixed_point fxp_neg(fixed_point);
fixed_point fxp_mul(int q, fixed_point, fixed_point);
fixed_point fxp_div(int q, fixed_point, fixed_point);
fixed_point fxp_inv(int q, fixed_point);

fixed_point fxp_addi(int q, fixed_point, int);
fixed_point fxp_subi(int q, fixed_point, int);
fixed_point fxp_muli(fixed_point, int);
fixed_point fxp_divi(fixed_point, int);
fixed_point fxp_invi(int q, int);

fixed_point fxp_isub(int q, int, fixed_point);
fixed_point fxp_idiv(int q, int, fixed_point);
fixed_point fxp_idivi(int q, int, int);

#endif /* lib/kernel/fixed-point.h */