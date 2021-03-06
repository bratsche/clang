// RUN: clang-cc -emit-llvm %s -o %t -arch=i386 &&
void t1(int len) {
  __asm__ volatile("" : "=&r"(len), "+&r"(len));
}

void t2(unsigned long long t)  {
  __asm__ volatile("" : "+m"(t));
}

void t3(unsigned char *src, unsigned long long temp) {
  __asm__ volatile("" : "+m"(temp), "+r"(src));
}

void t4() {
  unsigned long long a;
  struct reg { unsigned long long a, b; } b;

	__asm__ volatile ("":: "m"(a), "m"(b));
}

// PR3417
void t5(int i) {
  asm("nop" : "=r"(i) : "0"(t5));
}

// PR3641
void t6(void) {
  __asm__ volatile("" : : "i" (t6));
}

// RUN: grep "T7 NAMED: \$1" %t &&
void t7(int a) {
  __asm__ volatile("T7 NAMED: %[input]" : "+r"(a): [input] "i" (4));
}

// RUN: grep "T8 NAMED MODIFIER: \${0:c}" %t
void t8() {
  __asm__ volatile("T8 NAMED MODIFIER: %c[input]" :: [input] "i" (4));
}

// PR3682
unsigned t9(unsigned int a) {
  asm("bswap %0 %1" : "+r" (a));
  return a;
}

// PR3908
// RUN: grep "PR3908 \$1 \$3 \$2 \$0" %t
void t10(int r) {
  __asm__("PR3908 %[lf] %[xx] %[li] %[r]" : [r] "+r" (r) : [lf] "mx" (0), [li] "mr" (0), [xx] "x" ((double)(0)));
}         
