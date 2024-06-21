#include "lib/kernel/fixed-point.h"
#include <inttypes.h>

#define f(q) (1 << (q))

fixed_point fxp_of_int(int q, int n) {
    return n * f(q);
}

int fxp_trunc(int q, fixed_point x) {
    return x / f(q);
}

int fxp_round(int q, fixed_point x) {
    int sgn = (x >= 0) ? 1 : -1;
    return (x + sgn * f(q) / 2) / f(q);
}

fixed_point fxp_reformat(int old_q, int new_q, fixed_point x) {
    return ((int64_t) x) * f(new_q) / f(old_q);
}

fixed_point fxp_add(fixed_point x, fixed_point y) {
    return x + y;
}

fixed_point fxp_sub(fixed_point x, fixed_point y) {
    return x - y;
}

fixed_point fxp_neg(fixed_point x) {
    return -x;
}

fixed_point fxp_mul(int q, fixed_point x, fixed_point y) {
    return ((int64_t) x) * y / f(q);
}

fixed_point fxp_div(int q, fixed_point x, fixed_point y) {
    return ((int64_t) x) * f(q) / y;
}

fixed_point fxp_inv(int q, fixed_point x) {
    return ((int64_t) f(q)) * f(q) / x;
}

fixed_point fxp_addi(int q, fixed_point x, int n) {
    return x + n * f(q);
}

fixed_point fxp_subi(int q, fixed_point x, int n) {
    return x - n * f(q);
}

fixed_point fxp_muli(fixed_point x, int n) {
    return x * n;
}

fixed_point fxp_divi(fixed_point x, int n) {
    return x / n;
}

fixed_point fxp_invi(int q, int n) {
    return f(q) / n;
}

fixed_point fxp_isub(int q, int n, fixed_point x) {
    return n * f(q) - x;
}

fixed_point fxp_idiv(int q, int n, fixed_point x) {
    return ((int64_t) n) * f(q) * f(q) / x;
}

fixed_point fxp_idivi(int q, int n, int d) {
    return ((int64_t) n) * f(q) / d;
}