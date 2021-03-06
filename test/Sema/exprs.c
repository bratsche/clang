// RUN: clang-cc %s -verify -pedantic -fsyntax-only -fblocks=0

// PR1966
_Complex double test1() {
  return __extension__ 1.0if;
}

_Complex double test2() {
  return 1.0if;    // expected-warning {{imaginary constants are an extension}}
}

// rdar://6097308
void test3() {
  int x;
  (__extension__ x) = 10;
}

// rdar://6162726
void test4() {
      static int var;
      var =+ 5;  // expected-warning {{use of unary operator that may be intended as compound assignment (+=)}}
      var =- 5;  // expected-warning {{use of unary operator that may be intended as compound assignment (-=)}}
      var = +5;  // no warning when space between the = and +.
      var = -5;

      var =+5;  // no warning when the subexpr of the unary op has no space before it.
      var =-5;
  
#define FIVE 5
      var=-FIVE;  // no warning with macros.
      var=-FIVE;
}

// rdar://6319320
void test5(int *X, float *P) {
  (float*)X = P;   // expected-error {{assignment to cast is illegal, lvalue casts are not supported}}
#define FOO ((float*) X)
  FOO = P;   // expected-error {{assignment to cast is illegal, lvalue casts are not supported}}
}

void test6() {
  int X;
  X();  // expected-error {{called object type 'int' is not a function or function pointer}}
}

void test7(int *P, _Complex float Gamma) {
   P = (P-42) + Gamma*4;  // expected-error {{invalid operands to binary expression ('int *' and '_Complex float')}}
}


// rdar://6095061
int test8(void) {
  int i;
  __builtin_choose_expr (0, 42, i) = 10;  // expected-warning {{extension used}}
  return i;
}


// PR3386
struct f { int x : 4;  float y[]; };
int test9(struct f *P) {
  int R;
  R = __alignof(P->x);  // expected-error {{invalid application of '__alignof' to bitfield}} expected-warning {{extension used}}
  R = __alignof(P->y);   // ok. expected-warning {{extension used}}
  R = sizeof(P->x); // expected-error {{invalid application of 'sizeof' to bitfield}}
  return R;
}

// PR3562
void test10(int n,...) {
  struct S {
    double          a[n];  // expected-error {{fields must have a constant size}}
  }               s;
  double x = s.a[0];  // should not get another error here.
}


#define MYMAX(A,B)    __extension__ ({ __typeof__(A) __a = (A); __typeof__(B) __b = (B); __a < __b ? __b : __a; })

struct mystruct {int A; };
void test11(struct mystruct P, float F) {
  MYMAX(P, F);  // expected-error {{invalid operands to binary expression ('typeof(P)' (aka 'struct mystruct') and 'typeof(F)' (aka 'float'))}}
}

// PR3753
int test12(const char *X) {
  return X == "foo";  // expected-warning {{comparison against a string literal is unspecified}}
}

// rdar://6719156
void test13(
            void (^P)()) { // expected-error {{blocks support disabled - compile with -fblocks}}
  P();
  P = ^(){}; // expected-error {{blocks support disabled - compile with -fblocks}}
}


// rdar://6326239 - Vector comparisons are not fully trusted yet, until the
// backend is known to work, just unconditionally reject them.
void test14() {
  typedef long long __m64 __attribute__((__vector_size__(8))); // expected-warning {{extension used}}
  typedef short __v4hi __attribute__((__vector_size__(8))); // expected-warning {{extension used}}

  __v4hi a;
  __m64 mask = (__m64)((__v4hi)a >  // expected-error {{comparison of vector types ('__v4hi' and '__v4hi') not supported yet}}
                      (__v4hi)a);
}

