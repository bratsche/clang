// RUN: clang-cc %s -fsyntax-only -verify

typedef unsigned __uint32_t;

#define __byte_swap_int_var(x) \
__extension__ ({ register __uint32_t __X = (x); \
   __asm ("bswap %0" : "+r" (__X)); \
   __X; })

int test(int _x) {
 return (__byte_swap_int_var(_x));
}

// PR2374
int test2() { return ({L:5;}); }
int test3() { return ({ {5;} }); }         // expected-error {{incompatible type returning 'void', expected 'int'}}\
                                           // expected-warning {{expression result unused}}
int test4() { return ({ ({5;}); }); }
int test5() { return ({L1: L2: L3: 5;}); }
int test6() { return ({5;}); }
void test7() { ({5;}); }                   // expected-warning {{expression result unused}}

// PR3062
int x[({10;})]; // expected-error {{statement expression not allowed at file scope}}
