/*version of mlfqs_3*/
/*version of mlfqs_3*/
/*version of mlfqs_3*/

#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

#define F (1 << 14)

int int_to_fp (int);

int fp_to_int_zero (int);

int fp_to_int_nearest (int);

int add_x_y (int , int);

int sub_x_y (int , int);

int add_x_n (int , int);

int sub_x_n (int , int);

int mul_x_y (int , int);

int mul_x_n (int , int);

int div_x_y (int , int);

int div_x_n (int , int);

#endif