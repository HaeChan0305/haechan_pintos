/*version of mlfqs_2*/
/*version of mlfqs_2*/
/*version of mlfqs_2*/

#include "threads/fixed_point.h"

int
int_to_fp(int n){
    return n * F;
}

int
fp_to_int_zero(int x){
    return x / F;
}

int
fp_to_int_nearest(int x){
    return (x >= 0) ? (x + F/2) / F : (x - F/2) / F ; 
}

int
add_x_y(int x, int y){
    return x + y;
}

int
sub_x_y(int x, int y){
    return x - y;
}

int
add_x_n(int x, int n){
    return x + n * F;
}

int 
sub_x_n(int x, int n){
    return x - n * F;
}

int
mul_x_y(int x, int y){
    return ((int64_t) x) * y / F;
}

int 
mul_x_n(int x, int n){
    return x * n;
}

int
div_x_y(int x, int y){
    return ((int64_t) x) * F / y;
}

int
div_x_n(int x, int n){
    return x / n;
}